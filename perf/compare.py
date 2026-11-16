# what the end to end check and the workload runner share: splitting a SQL file into statements, loading a
# dataset into PostgreSQL, and comparing our rows with PostgreSQL's
import re, subprocess


def run(cmd, stdin=None):
    r = subprocess.run(cmd, capture_output=True, text=True, input=stdin)
    if r.returncode != 0: raise RuntimeError(' '.join(cmd) + '\n' + r.stdout + r.stderr)
    return r.stdout


def statements(path):
    found, current, quoted, text, i = [], '', False, open(path).read(), 0
    while i < len(text):
        c = text[i]
        if not quoted and text.startswith('--', i):
            i = text.find('\n', i) if '\n' in text[i:] else len(text)
            continue
        if c == "'": quoted = not quoted
        if c == ';' and not quoted:
            if current.strip(): found.append(current.strip() + ';')
            current = ''
        else:
            current += c
        i += 1
    if current.strip(): found.append(current.strip() + ';')
    return found


# our engine compares strings byte by byte, which is PostgreSQL's "C" collation; a server whose default
# collation is a language's (en_US.UTF-8 on most Linux installs) sorts 'apple' before 'Banana' and would
# disagree on ORDER BY, < and >, MIN and MAX of text for reasons that have nothing to do with correctness
def postgresSchema(schema):
    return re.sub(r'(VARCHAR\s*\(\s*\d+\s*\))', r'\1 COLLATE "C"', schema, flags=re.IGNORECASE)


def tablesOf(schema):
    return re.findall(r'CREATE\s+TABLE\s+(\w+)', schema, flags=re.IGNORECASE)


def loadPostgres(psql, schema, files):
    tables = tablesOf(schema)
    run(psql + ['-v', 'ON_ERROR_STOP=1', '-q', '-c', ' '.join('DROP TABLE IF EXISTS %s;' % t for t in tables)])
    run(psql + ['-v', 'ON_ERROR_STOP=1', '-q'], postgresSchema(schema))
    for table in tables:
        run(psql + ['-v', 'ON_ERROR_STOP=1', '-q', '-c', "\\copy %s FROM '%s' WITH (FORMAT csv)" % (table, files[table])])


def postgres(psql, query):
    out = run(psql + ['-v', 'ON_ERROR_STOP=1', '-X', '-A', '-t', '-F', '\t', '-c', query])
    return [line.split('\t') for line in out.split('\n') if line != '']


def number(v):
    try: return float(v)
    except ValueError: return None


def key(row):   # for sorting a bag: numbers by value, strings by text
    return tuple((0, number(v), '') if number(v) is not None else (1, 0, v) for v in row)


def agree(got, want):   # AVG is printed to 4 places and PostgreSQL's numeric is exact, so numbers may differ by 0.00005
    if len(got) != len(want): return False
    for g, w in zip(got, want):
        if len(g) != len(w): return False
        for a, b in zip(g, w):
            if number(a) is not None and number(b) is not None:
                if abs(number(a) - number(b)) > 1e-4: return False
            elif a != b: return False
    return True


def sameBag(got, want):
    return agree(sorted(got, key=key), sorted(want, key=key))
