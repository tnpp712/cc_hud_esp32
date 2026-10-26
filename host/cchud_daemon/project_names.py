"""只读解析 Codex 项目名称，优先使用会话归属，目录仅作无歧义回退。"""
import json
import sqlite3
from pathlib import Path


def clean_name(value):
    return ' '.join(value.split()) if isinstance(value, str) else ''


class ProjectNames:
    def __init__(self, home):
        self.home = Path(home)
        self.projects = {}
        self.assignments = {}
        self.legacy_projects = {}
        self.legacy_assignments = {}
        self.roots = []

    def refresh(self):
        projects, assignments, roots = {}, {}, []
        database = self.home/'state_5.sqlite'
        if database.exists():
            try:
                with sqlite3.connect(f'file:{database}?mode=ro', uri=True) as c:
                    tables = {r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                    if 'projects' in tables:
                        projects = {pid: clean_name(name) for pid,name in c.execute('SELECT id,name FROM projects')}
                    if 'project_roots' in tables:
                        roots = [(path,projects.get(pid,'')) for pid,path in c.execute('SELECT project_id,path FROM project_roots')]
                    columns = {r[1] for r in c.execute('PRAGMA table_info(threads)')}
                    if {'id','project_id'} <= columns:
                        assignments = dict(c.execute('SELECT id,project_id FROM threads WHERE project_id IS NOT NULL'))
            except (sqlite3.Error, OSError):
                projects, assignments, roots = {}, {}, []
        legacy_projects, legacy_assignments = {}, {}
        try:
            data = json.loads((self.home/'.codex-global-state.json').read_text())
            local = data.get('local-projects', {})
            if isinstance(local, dict):
                for pid,project in local.items():
                    if not isinstance(project, dict):
                        continue
                    name = clean_name(project.get('name'))
                    legacy_projects[pid] = name
                    paths = project.get('rootPaths', [])
                    if isinstance(paths, list):
                        roots.extend((path,name) for path in paths if isinstance(path,str))
            raw = data.get('thread-project-assignments', {})
            if isinstance(raw, dict):
                legacy_assignments = {sid: item.get('projectId') for sid,item in raw.items()
                                      if isinstance(item,dict) and item.get('projectKind')=='local'}
        except (OSError, ValueError, AttributeError):
            pass
        self.projects, self.assignments = projects, assignments
        self.legacy_projects, self.legacy_assignments = legacy_projects, legacy_assignments
        self.roots = [(Path(path).resolve(), name) for path,name in roots
                      if isinstance(path,str) and path and name]

    def name(self, sid, cwd):
        name = self.projects.get(self.assignments.get(sid), '')
        if name:
            return name
        name = self.legacy_projects.get(self.legacy_assignments.get(sid), '')
        if name:
            return name
        if not cwd:
            return ''
        path = Path(cwd).resolve()
        matches = [(len(root.parts),name) for root,name in self.roots if path.is_relative_to(root)]
        if not matches:
            return ''
        depth = max(length for length,_ in matches)
        names = {name for length,name in matches if length==depth}
        return names.pop() if len(names)==1 else ''
