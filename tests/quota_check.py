#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 Douglas P. Fields, Jr. (symbolics@lisp.engineer) (see LICENSE)
"""quota_check.py - a volume with ACTIVE disk quotas is not written (CORPUS 71/72: VMS marks quotas active ONLY by the existence of [000000]QUOTA.SYS; there is no home-block flag, DISKQUOTA DISABLE
changes nothing on disk, and MOUNT enables quotas whenever the file exists). ods2v2 does not keep QUOTA.SYS, so a write would leave its usage figures wrong. Every write is refused unless --ignore-quota
is given; reads are unaffected; QUOTA.SYS itself is never deleted, renamed, linked or given a new version (even with the override: VMS refuses a DELETE while quotas are active). At ODS-2, ODS-5 and
ODS-5 HARDLINKS. The quota volume is made by a host patch (tests/quota_volume.py): ods2v2 makes a file QUOTX.SYS in [000000] and the directory record's name is changed to QUOTA.SYS in place (a
same-length name), because ods2v2 itself refuses to make that name. Run from the project root (./ods2, ./ods2_init, ./ods2_put, ./ods2_rm, ./ods2_mkdir built)."""
import hashlib
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quota_volume as Q   # noqa: E402

KINDS = (('ODS-2', 2, False), ('ODS-5', 5, False), ('ODS-5 HARDLINKS', 5, True))
passes = fails = 0
MSG = 'disk quotas are active'


def check(desc, expected, got):
    global passes, fails
    if expected == got:
        passes += 1
        print('PASS: ' + desc)
    else:
        fails += 1
        print('FAIL: ' + desc)
        print('      expected: %r' % (expected,))
        print('      got:      %r' % (got,))


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def fresh(base, work, tag):
    c = os.path.join(work, 'w_%s.img' % tag)
    shutil.copyfile(base, c)
    return c


def lock_left(img):
    return os.path.exists(img + '.lock')


def writes(work):
    """(tag, command with the host file names filled in, needs ODS-5): one case for each write command of the CLI"""
    f = os.path.join(work, 'host.txt')
    d = os.path.join(work, 'hostdir')
    return [
        ('copy', 'COPY %s [A]NEW.TXT' % f, False),
        ('copy_new_version', 'COPY %s [A]F.TXT' % f, False),
        ('copy_convert', 'COPY/CONVERT %s [A]NEWC.TXT' % f, False),
        ('create_directory', 'CREATE/DIRECTORY [B]', False),
        ('delete', 'DELETE [A]F.TXT;1', False),
        ('delete_wild', 'DELETE [A]*.TXT;*', False),
        ('purge', 'PURGE [A]F.TXT', False),
        ('rename', 'RENAME [A]G.TXT [A]G2.TXT', False),
        ('link', 'LINK [A]L.TXT [A]F.TXT', True),
        ('symlink', 'SYMLINK [A]S.TXT [A]F.TXT', True),
        ('set_protection', 'SET FILE/PROTECTION=(W:R) [A]F.TXT', False),
        ('set_owner', 'SET FILE/OWNER_UIC=[1,4] [A]F.TXT', False),
        ('set_version_limit', 'SET FILE/VERSION_LIMIT=3 [A]F.TXT', False),
        ('set_volume_label', 'SET VOLUME/LABEL=NEWLAB', False),
        ('import', 'IMPORT %s [A]' % d, False),
    ]


def review74(work, kind, level, hl):
    host = os.path.join(work, 'host.txt')
    # F1 fail closed: the master directory cannot be read to the end (a damaged record before or after QUOTA.SYS, or at it): the guard cannot tell, so the write mount is refused, with its own message,
    # and --ignore-quota overrides it. Reads work (DIR lists what it can).
    for where, dmg in (('after QUOTA.SYS (TOP.TXT)', 'TOP.TXT'), ('before QUOTA.SYS (BACKUP.SYS)', 'BACKUP.SYS'), ('at QUOTA.SYS', 'QUOTA.SYS')):
        im = Q.make(work, level, hl, True, 'f1', top=True)
        Q.damage_record(im, '[000000]', dmg)
        h0 = sha(im)
        rc, out = Q.ods2(im, 'COPY %s [A]N.TXT' % host)
        check('%s F1 damaged record %s: the write is refused (it cannot be told whether quotas are active), the image unchanged, the message says the master directory could not be read and names --ignore-quota' % (kind, where),
              (True, True, h0), ('could not be read to look for' in out and '--ignore-quota' in out, 'ODS2-I-COPIED' not in out, sha(im)))
        rc, out = Q.ods2(im, 'DIR [A]')
        check('%s F1 damaged record %s: a read still works' % (kind, where), True, 'F.TXT' in out and 'ODS2-F' not in out)
        rc, out = Q.ods2(im, 'COPY %s [A]N.TXT' % host, ['--ignore-quota'])
        check('%s F1 damaged record %s: --ignore-quota writes' % (kind, where), True, 'ODS2-I-COPIED' in out and sha(im) != h0)
        rc, out = Q.run(['./ods2_put', im, host, 'A', 'P.TXT'])
        check('%s F1 damaged record %s: ods2_put exits 1 with the same reason' % (kind, where), (1, True), (rc, 'could not be read to look for' in out))
    # S1: commands on standard input: exit 1 when any command failed (as @script does), also when nothing is a quota
    im = Q.make(work, level, hl, True, 's1')
    p = __import__('subprocess').run(['./ods2', im], input='COPY %s [A]X.TXT\nDIR [A]\n' % host, capture_output=True, text=True)
    check('%s S1: a write refused on a quota volume, commands on standard input: exit 1' % kind, 1, p.returncode)
    im = Q.make(work, level, hl, False, 's1p')
    p = __import__('subprocess').run(['./ods2', im], input='DELETE [A]NOSUCH.TXT;1\nDIR [A]\n', capture_output=True, text=True)
    check('%s S1: a failing DELETE on a plain volume, commands on standard input: exit 1' % kind, 1, p.returncode)
    p = __import__('subprocess').run(['./ods2', im], input='DIR [A]\nDELETE [A]G.TXT;1\n', capture_output=True, text=True)
    check('%s S1: commands on standard input that all succeed: exit 0' % kind, 0, p.returncode)
    # N1: SET FILE on QUOTA.SYS is refused even with the override (DISKQUOTA owns the file)
    quota = Q.make(work, level, hl, True, 'n1')
    for what in ('SET FILE/VERSION_LIMIT=1 [000000]QUOTA.SYS', 'SET FILE/PROTECTION=(W:R) [000000]QUOTA.SYS', 'SET FILE/OWNER_UIC=[1,4] [000000]QUOTA.SYS', 'SET FILE/ATTRIBUTES=(MRS:0) [000000]QUOTA.SYS'):
        im = fresh(quota, work, 'n1')
        h0 = sha(im)
        rc, out = Q.ods2(im, what, ['--ignore-quota'])
        check('%s N1: %s is refused with --ignore-quota (QUOTA.SYS named), the image unchanged' % (kind, what.split()[1].split('=')[0]), (True, h0), ('QUOTA.SYS' in out and ('-E-' in out or '-W-' in out), sha(im)))
    # N2: a dangling QUOTA.SYS entry (it names no file): read only without the override; with it DELETE removes the entry only, and then the volume is written without the option
    im = Q.make(work, level, hl, True, 'n2')
    Q.dangle_quota(im)
    h0 = sha(im)
    rc, out = Q.ods2(im, 'COPY %s [A]N.TXT' % host)
    check('%s N2: a dangling QUOTA.SYS entry still makes the volume read only' % kind, (True, h0), (MSG in out, sha(im)))
    rc, out = Q.ods2(im, 'DELETE [000000]QUOTA.SYS;1', ['--ignore-quota'])
    rc2, out2 = Q.ods2(im, 'DIR [000000]QUOTA.SYS')
    check('%s N2: DELETE of the dangling entry with --ignore-quota removes the entry' % kind, (True, True), ('ODS2-E' not in out and sha(im) != h0, 'QUOTA.SYS' not in out2.replace('QUOTA.SYS;1: no', '')))
    rc, out = Q.ods2(im, 'COPY %s [A]N.TXT' % host)
    check('%s N2: afterwards the volume is written without the option' % kind, (True, False), ('ODS2-I-COPIED' in out, MSG in out))
    # N6: a lower-case entry (ODS-5) is the quota file too; an alias entry of the quota file is refused by its file number
    if level == 5:
        im = Q.make(work, level, hl, True, 'n6l', lower=True)
        h0 = sha(im)
        rc, out = Q.ods2(im, 'COPY %s [A]N.TXT' % host)
        check('%s N6: an entry quota.sys (lower case) is the quota file: read only' % kind, (True, h0), (MSG in out, sha(im)))
    im = Q.make(work, level, hl, True, 'n6a', alias=True)
    h0 = sha(im)
    for what in ('DELETE [A]ALIAS.SYS;1', 'RENAME [A]ALIAS.SYS [A]OTHER.SYS', 'LINK [A]L2.SYS [A]ALIAS.SYS'):
        if what.startswith('LINK') and level != 5:
            continue
        im = Q.make(work, level, hl, True, 'n6a', alias=True)
        h0 = sha(im)
        rc, out = Q.ods2(im, what, ['--ignore-quota'])
        check('%s N6: %s (an alias entry of the quota file) is refused by the file number, with the override, the image unchanged' % (kind, what.split()[0]), (True, h0), ('QUOTA.SYS' in out and ('-E-' in out or '-W-' in out), sha(im)))


def main():
    work = tempfile.mkdtemp(prefix='quota_', dir=os.environ.get('TMPDIR', 'samples'))
    try:
        open(os.path.join(work, 'host.txt'), 'w').write('host file\n')
        os.makedirs(os.path.join(work, 'hostdir'))
        open(os.path.join(work, 'hostdir', 'I.TXT'), 'w').write('imported\n')
        for kind, level, hl in KINDS:
            print('=== %s' % kind)
            plain = Q.make(work, level, hl, False, 'plain')
            quota = Q.make(work, level, hl, True, 'quota')
            decoy = Q.make(work, level, hl, False, 'decoy', decoy=True)
            for tag, cmd, needs5 in writes(work):
                if needs5 and level != 5:
                    continue
                # a control: the same command on a volume with no quotas works
                im = fresh(plain, work, 'c')
                rc, out = Q.ods2(im, cmd)
                check('%s %s: on a volume with no quotas the command works (%s)' % (kind, tag, cmd.split()[0]), (True, False), (sha(im) != sha(plain), MSG in out))
                # refused: the message says why, advises DISKQUOTA REBUILD and the override; nothing written
                im = fresh(quota, work, 'q')
                h0 = sha(im)
                rc, out = Q.ods2(im, cmd)
                check('%s %s: refused on a volume with active quotas: the reason, --ignore-quota and DISKQUOTA REBUILD in the message, the image byte for byte unchanged, no lock left' % (kind, tag),
                      (True, True, True, h0, False), (MSG in out, '--ignore-quota' in out, 'DISKQUOTA REBUILD' in out, sha(im), lock_left(im)))
                # with the override the command does what it does on a plain volume
                im = fresh(quota, work, 'o')
                rc, out = Q.ods2(im, cmd, ['--ignore-quota'])
                check('%s %s: with --ignore-quota the command works' % (kind, tag), (True, False), (sha(im) != h0, MSG in out))
            # a decoy: QUOTA.SYS in another directory, QUOTA.DAT in the MFD: no quotas, writes work
            im = fresh(decoy, work, 'd')
            rc, out = Q.ods2(im, 'COPY %s [A]NEW.TXT' % os.path.join(work, 'host.txt'))
            check('%s: [A]QUOTA.SYS and [000000]QUOTA.DAT are not the quota file: a write goes through, no message' % kind, (True, False), (sha(im) != sha(decoy), MSG in out))
            # reads work and say nothing about quotas beyond the one mount warning
            im = fresh(quota, work, 'r')
            h0 = sha(im)
            for rcmd in ('DIR [A]', 'TYPE [A]F.TXT', 'DIR/FULL [A]F.TXT', 'SHOW VOLUME', 'EXPORT [A]G.TXT %s' % tempfile.mkdtemp(prefix='exp', dir=work)):
                rc, out = Q.ods2(im, rcmd)
                check('%s: %s reads on a volume with active quotas' % (kind, rcmd.split()[0]), (True, True), ('ODS2-E' not in out and 'ILLCMD' not in out and 'DIRERR' not in out, out.count(MSG) <= 1))
            check('%s: the reads changed nothing' % kind, h0, sha(im))
            rc, out = Q.ods2(im, 'DIR [A]')
            check('%s: the mount says it once, as a warning, in the form of the other read-only mounts' % kind, (True, True), ('%ODS2-W-READONLY' in out, 'mounted read-only' in out))
            # a script: the first write is refused, a later read still works, the exit status says an error
            script = os.path.join(work, 'script.txt')
            open(script, 'w').write('DIR [A]\nCOPY %s [A]X.TXT\nDIR [A]\n' % os.path.join(work, 'host.txt'))
            im = fresh(quota, work, 's')
            rc, out = Q.run(['./ods2', im, '@' + script])
            check('%s: a script on the quota volume: the COPY is refused, the reads around it work, the image is unchanged' % kind, (True, True, h0), (MSG in out, out.count('F.TXT') >= 2, sha(im)))
            # the stand-alone tools: exit 1, the message, nothing written; with the override they write
            tools = [('ods2_put', ['./ods2_put', '%IMG%', os.path.join(work, 'host.txt'), 'A', 'P.TXT']), ('ods2_rm', ['./ods2_rm', '%IMG%', 'A', 'F.TXT;1']),
                     ('ods2_mkdir', ['./ods2_mkdir', '%IMG%', '', 'NEWDIR'])]
            for tname, argv in tools:
                im = fresh(quota, work, 't')
                h0 = sha(im)
                rc, out = Q.run([a.replace('%IMG%', im) for a in argv])
                check('%s %s: refused, exit 1, the message with --ignore-quota and DISKQUOTA REBUILD, the image unchanged' % (kind, tname), (1, True, True, True, h0),
                      (rc, MSG in out, '--ignore-quota' in out, 'DISKQUOTA REBUILD' in out, sha(im)))
                rc, out = Q.run([argv[0], '--ignore-quota'] + [a.replace('%IMG%', im) for a in argv[1:]])
                check('%s %s --ignore-quota writes (exit 0)' % (kind, tname), (0, True), (rc, sha(im) != h0))
                im = fresh(plain, work, 't2')
                rc, out = Q.run([a.replace('%IMG%', im) for a in argv])
                check('%s %s on a volume with no quotas works (exit 0)' % (kind, tname), 0, rc)
            # the checker: a quota volume is a valid volume, no new finding
            im = fresh(quota, work, 'k')
            rc, out = Q.run(['python3', '-I', 'tests/ods5check.py', im])
            base_rc, base_out = Q.run(['python3', '-I', 'tests/ods5check.py', plain])
            check('%s: ods5check finds the quota volume as sound as the plain one (no errors, no warnings)' % kind, ([], []),
                  ([l for l in out.splitlines() if l.startswith('ERROR')], [l for l in out.splitlines() if l.startswith('WARN')]))
            # QUOTA.SYS itself, with the override: not deleted, not renamed, not linked, not given a new version, nothing made under its name
            guards = [('DELETE', 'DELETE [000000]QUOTA.SYS;1'), ('DELETE wildcard', 'DELETE [000000]QUOTA.*;*'), ('PURGE', 'PURGE [000000]QUOTA.SYS'),
                      ('COPY onto it', 'COPY %s [000000]QUOTA.SYS' % os.path.join(work, 'host.txt')), ('RENAME it away', 'RENAME [000000]QUOTA.SYS [A]Q.SYS'),
                      ('RENAME onto it', 'RENAME [A]G.TXT [000000]QUOTA.SYS'), ('SET FILE/ENTER', None)]
            if level == 5:
                guards.append(('LINK to it', 'LINK [A]QL.SYS [000000]QUOTA.SYS'))
            for gname, gcmd in guards:
                if gcmd is None:
                    continue
                im = fresh(quota, work, 'g')
                h0 = sha(im)
                rc, out = Q.ods2(im, gcmd, ['--ignore-quota'])
                # (PURGE of a name with one version has nothing to purge: nothing is said, nothing changes)
                check('%s: %s: even with --ignore-quota the quota file is protected (message names QUOTA.SYS), the image is unchanged' % (kind, gname), (True, h0),
                      ('QUOTA.SYS' in out and ('%ODS2-E' in out or '-E-' in out or '-W-' in out) or gname == 'PURGE', sha(im)))
            review74(work, kind, level, hl)
            # a volume with no quotas: nothing may be made under that name either (ods2v2 does not make a quota file)
            im = fresh(plain, work, 'n')
            h0 = sha(im)
            rc, out = Q.ods2(im, 'COPY %s [000000]QUOTA.SYS' % os.path.join(work, 'host.txt'))
            check('%s: no QUOTA.SYS is made on a volume that has none (ods2v2 does not keep quota files), the image is unchanged' % kind, (True, h0), ('QUOTA.SYS' in out, sha(im)))
            rc, out = Q.ods2(im, 'COPY %s [A]QUOTA.SYS' % os.path.join(work, 'host.txt'))
            check('%s: a QUOTA.SYS in another directory is an ordinary file' % kind, True, sha(im) != h0)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print('quota_check: %d passed, %d FAILED' % (passes, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
