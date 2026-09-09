from __future__ import annotations

import json
from pathlib import Path
import sqlite3
from contextlib import closing

from defectguard.domain import AnalysisReport


SCHEMA = """
CREATE TABLE IF NOT EXISTS analysis_runs (
    run_id TEXT PRIMARY KEY,
    created_at TEXT NOT NULL,
    source_root TEXT NOT NULL,
    source_fingerprint TEXT NOT NULL,
    finding_count INTEGER NOT NULL,
    report_json TEXT NOT NULL,
    verification_status TEXT NOT NULL DEFAULT 'skipped',
    source_unchanged INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS findings (
    run_id TEXT NOT NULL,
    rule_id TEXT NOT NULL,
    severity TEXT NOT NULL,
    file TEXT NOT NULL,
    line INTEGER NOT NULL,
    column_no INTEGER NOT NULL,
    message TEXT NOT NULL,
    detector TEXT NOT NULL DEFAULT 'rule',
    confidence REAL NOT NULL DEFAULT 1.0,
    FOREIGN KEY(run_id) REFERENCES analysis_runs(run_id)
);
"""


class RunStore:
    def __init__(self, path: Path) -> None:
        self.path = path

    def save(self, report: AnalysisReport) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(self.path)) as connection:
            with connection:
                connection.executescript(SCHEMA)
                self._ensure_columns(connection)
                connection.execute(
                    """INSERT OR REPLACE INTO analysis_runs
                    (run_id, created_at, source_root, source_fingerprint, finding_count,
                     report_json, verification_status, source_unchanged)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?)""",
                    (
                        report.run_id,
                        report.created_at,
                        report.source_root,
                        report.source_fingerprint,
                        len(report.findings),
                        json.dumps(report.to_dict(), ensure_ascii=False),
                        report.verification.status,
                        int(report.source_unchanged),
                    ),
                )
                connection.execute("DELETE FROM findings WHERE run_id = ?", (report.run_id,))
                connection.executemany(
                    """INSERT INTO findings
                    (run_id, rule_id, severity, file, line, column_no, message, detector, confidence)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                    [
                        (
                            report.run_id,
                            item.rule_id,
                            item.severity.value,
                            item.location.file,
                            item.location.line,
                            item.location.column,
                            item.message,
                            item.detector,
                            item.confidence,
                        )
                        for item in report.findings
                    ],
                )

    @staticmethod
    def _ensure_columns(connection: sqlite3.Connection) -> None:
        run_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(analysis_runs)")
        }
        finding_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(findings)")
        }
        if "verification_status" not in run_columns:
            connection.execute(
                "ALTER TABLE analysis_runs ADD COLUMN verification_status TEXT NOT NULL DEFAULT 'skipped'"
            )
        if "source_unchanged" not in run_columns:
            connection.execute(
                "ALTER TABLE analysis_runs ADD COLUMN source_unchanged INTEGER NOT NULL DEFAULT 1"
            )
        if "detector" not in finding_columns:
            connection.execute(
                "ALTER TABLE findings ADD COLUMN detector TEXT NOT NULL DEFAULT 'rule'"
            )
        if "confidence" not in finding_columns:
            connection.execute(
                "ALTER TABLE findings ADD COLUMN confidence REAL NOT NULL DEFAULT 1.0"
            )
