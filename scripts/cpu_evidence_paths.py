"""Resolve archive-relative capture and artifact paths for offline verification."""
import os
from pathlib import Path


def capture_path(value):
    path=Path(value)
    if path.is_absolute():return path
    root=os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
    return Path(root)/path if root else path


def artifact_path(value):
    path=Path(value)
    if path.is_absolute():return path
    root=os.environ.get('ARC_COMPOSITE_ARTIFACT_ROOT')
    return Path(root)/path if root else path
