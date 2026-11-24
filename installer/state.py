"""totk-install.json in the install folder: what built it and which steps finished.

    {"format": 1, "release": "...", "complete": true, "lang": "en-US",
     "inputs": {"base": {"name", "size", "sha256"}, "update": {...}},
     "steps": {"<step>": {"key": "<input fingerprint>", "release": "...", "finished": "<UTC time>", "seconds": 12.3,
                          ...the step's own fields}}}

A step's key fingerprints everything its output depends on (runner.py), so a step whose recorded key equals the
current one and whose output still checks out is not run again. The file is replaced atomically after each step.
No key value or keys file content is ever stored.

"complete" says a setup run on this folder has finished: false from the start of a first install until its first run
ends well, true from then on, also while a later update or rebuild runs or after it failed (a game was built here
once; a run that failed after it replaced files can leave it broken until the setup runs again). "release" is the
release of the last run that finished. A file written before the entry
existed has none. Programs that start the game for the player read both (docs/installer.md, "Starting the game from
another program").
"""
import json
import os
from pathlib import Path

NAME = 'totk-install.json'
FORMAT = 1


class StateError(Exception):
    def __init__(self, id, path):
        super().__init__(id)
        self.id, self.path = id, path


def empty(release):
    return {'format': FORMAT, 'release': release, 'complete': False, 'inputs': {}, 'steps': {}}


def load(dest):
    """The state of dest, or None when the folder has none. StateError for an unreadable or newer file."""
    path = Path(dest) / NAME
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        raise StateError('state.unreadable', path) from None
    if not isinstance(data, dict) or not isinstance(data.get('steps'), dict):
        raise StateError('state.unreadable', path)
    if data.get('format', 0) > FORMAT:
        raise StateError('state.newer_format', path)
    return data


def save(dest, data):
    path = Path(dest) / NAME
    tmp = path.with_name(NAME + '.tmp')
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
