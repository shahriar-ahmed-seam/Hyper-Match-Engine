//! Append-only persistence journal (write-ahead log) for accepted mutating
//! commands.
//!
//! Each line is one JSON record, written only after the engine confirms the
//! command with its terminating `Ack`. Because the matching engine is
//! deterministic, replaying the accepted-command history in order reconstructs
//! the identical order book — so the journal lets the book survive engine or
//! gateway restarts even though the engine itself is purely in-memory.

use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use codec::Side;
use serde::{Deserialize, Serialize};

/// A single persisted command. Serialized as one JSON object per line.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum JournalRecord {
    /// An accepted new order, carrying the fields needed to re-submit it.
    New {
        order_id: u64,
        side: Side,
        price_ticks: u64,
        quantity: u32,
    },
    /// An accepted cancellation of a resting order.
    Cancel { order_id: u64 },
}

/// An append-only journal backed by a single file opened in append mode.
#[derive(Debug)]
pub struct Journal {
    path: PathBuf,
    file: Mutex<File>,
}

impl Journal {
    /// Open (creating if missing) the journal file at `path` in append mode,
    /// creating any missing parent directories.
    pub fn open(path: impl AsRef<Path>) -> std::io::Result<Journal> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                std::fs::create_dir_all(parent)?;
            }
        }
        let file = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)?;
        Ok(Journal {
            path,
            file: Mutex::new(file),
        })
    }

    /// Append a record as a single JSON line and flush it to the OS.
    ///
    /// Persistence is best-effort: a write failure is logged rather than
    /// propagated so a journal problem never blocks serving traffic.
    pub fn append(&self, record: &JournalRecord) {
        let mut line = match serde_json::to_string(record) {
            Ok(line) => line,
            Err(err) => {
                tracing::error!(error = %err, "failed to serialize journal record");
                return;
            }
        };
        line.push('\n');
        let mut file = self.file.lock().unwrap();
        if let Err(err) = file.write_all(line.as_bytes()).and_then(|_| file.flush()) {
            tracing::error!(error = %err, "failed to append to journal");
        }
    }

    /// Read and parse every record in the journal, in order.
    ///
    /// Blank lines are skipped; a line that fails to parse aborts the read with
    /// an error so a corrupt journal is surfaced rather than silently dropping
    /// history.
    pub fn read_records(&self) -> std::io::Result<Vec<JournalRecord>> {
        read_records(&self.path)
    }
}

/// Read and parse every record from a journal file at `path`, in order.
pub fn read_records(path: impl AsRef<Path>) -> std::io::Result<Vec<JournalRecord>> {
    let file = match File::open(path.as_ref()) {
        Ok(file) => file,
        // A missing journal is simply an empty history.
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(err) => return Err(err),
    };
    let reader = BufReader::new(file);
    let mut records = Vec::new();
    for line in reader.lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        let record: JournalRecord = serde_json::from_str(&line)
            .map_err(|err| std::io::Error::new(std::io::ErrorKind::InvalidData, err))?;
        records.push(record);
    }
    Ok(records)
}

#[cfg(test)]
mod tests {
    use super::*;
    use codec::BinaryMessage;

    /// Mirror of the replay command-building: a record maps to the wire
    /// command that re-submits it (a `NewOrder` stamped with `seq`, or a
    /// `CancelOrder`).
    fn to_command(record: &JournalRecord, seq: u64) -> BinaryMessage {
        match *record {
            JournalRecord::New {
                order_id,
                side,
                price_ticks,
                quantity,
            } => BinaryMessage::NewOrder {
                order_id,
                side,
                price_ticks,
                quantity,
                seq,
            },
            JournalRecord::Cancel { order_id } => BinaryMessage::CancelOrder { order_id },
        }
    }

    fn temp_path(name: &str) -> PathBuf {
        let mut path = std::env::temp_dir();
        path.push(format!("hme-journal-test-{}-{}.jsonl", std::process::id(), name));
        let _ = std::fs::remove_file(&path);
        path
    }

    #[test]
    fn append_then_read_round_trips_in_order() {
        let path = temp_path("roundtrip");
        let journal = Journal::open(&path).expect("open");
        journal.append(&JournalRecord::New {
            order_id: 1,
            side: Side::Buy,
            price_ticks: 10_000,
            quantity: 5,
        });
        journal.append(&JournalRecord::New {
            order_id: 2,
            side: Side::Sell,
            price_ticks: 10_100,
            quantity: 3,
        });
        journal.append(&JournalRecord::Cancel { order_id: 1 });

        let records = journal.read_records().expect("read");
        assert_eq!(
            records,
            vec![
                JournalRecord::New {
                    order_id: 1,
                    side: Side::Buy,
                    price_ticks: 10_000,
                    quantity: 5,
                },
                JournalRecord::New {
                    order_id: 2,
                    side: Side::Sell,
                    price_ticks: 10_100,
                    quantity: 3,
                },
                JournalRecord::Cancel { order_id: 1 },
            ]
        );
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn records_build_the_expected_ordered_command_list() {
        let records = vec![
            JournalRecord::New {
                order_id: 7,
                side: Side::Sell,
                price_ticks: 25_000,
                quantity: 9,
            },
            JournalRecord::New {
                order_id: 8,
                side: Side::Buy,
                price_ticks: 24_900,
                quantity: 4,
            },
            JournalRecord::Cancel { order_id: 7 },
        ];

        let commands: Vec<BinaryMessage> = records
            .iter()
            .enumerate()
            .map(|(i, record)| to_command(record, i as u64))
            .collect();

        assert_eq!(
            commands,
            vec![
                BinaryMessage::NewOrder {
                    order_id: 7,
                    side: Side::Sell,
                    price_ticks: 25_000,
                    quantity: 9,
                    seq: 0,
                },
                BinaryMessage::NewOrder {
                    order_id: 8,
                    side: Side::Buy,
                    price_ticks: 24_900,
                    quantity: 4,
                    seq: 1,
                },
                BinaryMessage::CancelOrder { order_id: 7 },
            ]
        );
    }

    #[test]
    fn records_survive_reopening_in_append_mode() {
        let path = temp_path("reopen");
        {
            let journal = Journal::open(&path).expect("open");
            journal.append(&JournalRecord::New {
                order_id: 1,
                side: Side::Buy,
                price_ticks: 5_000,
                quantity: 2,
            });
        }
        {
            let journal = Journal::open(&path).expect("reopen");
            journal.append(&JournalRecord::Cancel { order_id: 1 });
        }

        let records = read_records(&path).expect("read");
        assert_eq!(records.len(), 2);
        assert_eq!(records[1], JournalRecord::Cancel { order_id: 1 });
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn missing_journal_reads_as_empty() {
        let path = temp_path("missing");
        let records = read_records(&path).expect("read");
        assert!(records.is_empty());
    }

    #[test]
    fn serialized_form_matches_wire_documentation() {
        let new = serde_json::to_string(&JournalRecord::New {
            order_id: 42,
            side: Side::Buy,
            price_ticks: 12_345,
            quantity: 10,
        })
        .unwrap();
        assert_eq!(
            new,
            r#"{"type":"new","order_id":42,"side":"buy","price_ticks":12345,"quantity":10}"#
        );

        let cancel = serde_json::to_string(&JournalRecord::Cancel { order_id: 42 }).unwrap();
        assert_eq!(cancel, r#"{"type":"cancel","order_id":42}"#);
    }
}
