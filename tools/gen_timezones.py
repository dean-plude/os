#!/usr/bin/env python3
"""gen_timezones.py — build kernel/ke/tzdata.inc, NovaOS's time zone table

Each row is one Windows time zone (the names programs see in the registry
and in GetDynamicTimeZoneInformation) with its rule in Windows' own form
(Bias, StandardBias, DaylightBias, StandardDate, DaylightDate: the TZI
value under HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones).

Where the data comes from:
  - the Windows zone names and the cities behind each one: Unicode CLDR
    supplemental/windowsZones.xml (Unicode License v3);
  - the offsets and daylight-saving rules: the IANA time zone database
    (public domain), read through Python's zoneinfo (the tzdata package
    when installed, else the system's), taken for the year
    given (--year) and checked against the years around it.

    python3 tools/gen_timezones.py
    python3 tools/gen_timezones.py --cldr windowsZones.xml --year 2026

Without --cldr it downloads windowsZones.xml from the CLDR release named
in CLDR_URL.  The output names both versions in its header.
"""

import argparse
import calendar
import datetime as dt
import os
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET
import zoneinfo

CLDR_URL = 'https://raw.githubusercontent.com/unicode-org/cldr/release-47/common/supplemental/windowsZones.xml'
OUT = os.path.join(os.path.dirname(__file__), '..', 'kernel', 'ke', 'tzdata.inc')
UTC = dt.timezone.utc

# Territories by population (largest first): a zone's display name lists
# the cities of the most populous places that use it
TERR_RANK = '''IN CN US ID PK NG BR BD RU ET MX JP EG PH CD VN IR TR DE TH GB TZ FR ZA IT
KE MM CO KR UG SD ES DZ IQ AR AF YE CA AO UA MA PL UZ MY MZ GH PE SA MG CI CM NP VE NE
AU KP SY ML BF TW LK MW ZM CL KZ RO TD EC SO GT SN NL KH ZW GN RW BJ BI TN BO HT BE JO
DO CU SS SE HN CZ AZ GR PG PT HU TJ AE BY IL TG AT CH SL LA HK SV NI LY PY KG BG TM SG
DK FI CG SK NO PS CR LR OM IE NZ CF MR PA KW HR GE MN ER UY BA PR AM QA AL LT JM NA GM
BW GA LS SI MK LV GW GQ TT BH EE TL MU CY SZ DJ FJ RE KM GY BT SB MO LU ME SR IS MT BN
BS BZ'''.split()


def use_tzdata_package():
    """Prefer the tzdata package (pip install tzdata): it has the old zone
    names CLDR still uses (America/Indianapolis) that some systems drop"""
    try:
        import tzdata  # noqa: F401
        zoneinfo.reset_tzpath([])
    except ImportError:
        pass


def tzdata_version():
    if not zoneinfo.TZPATH:
        import tzdata
        return tzdata.IANA_VERSION
    for p in zoneinfo.TZPATH:
        try:
            with open(os.path.join(p, 'tzdata.zi')) as f:
                m = re.match(r'# version (\S+)', f.readline())
                if m:
                    return m.group(1)
        except OSError:
            pass
    try:
        import tzdata
        return tzdata.IANA_VERSION
    except ImportError:
        return 'unknown'


def transitions(zone, year):
    """(utc instant, offset before, offset after) for each change in @year"""
    out = []
    t = dt.datetime(year, 1, 1, tzinfo=UTC)
    end = dt.datetime(year + 1, 1, 1, tzinfo=UTC)
    prev = t.astimezone(zone).utcoffset()
    step = dt.timedelta(hours=1)
    while t < end:
        n = t + step
        off = n.astimezone(zone).utcoffset()
        if off != prev:                          # narrow it down to the minute
            lo, hi = t, n
            while hi - lo > dt.timedelta(minutes=1):
                mid = lo + (hi - lo) / 2
                mid = mid.replace(second=0, microsecond=0)
                if mid <= lo:
                    mid = lo + dt.timedelta(minutes=1)
                if mid.astimezone(zone).utcoffset() == prev:
                    lo = mid
                else:
                    hi = mid
            out.append((hi, prev, off))
            prev = off
        t = n
    return out


def minutes(td):
    return int(td.total_seconds() // 60)


def windows_date(local, last):
    """A transition's local time as Windows' SYSTEMTIME rule: month,
    weekday, week of the month (5 = the last), time of day"""
    week = 5 if last else (local.day - 1) // 7 + 1
    return (local.month, (local.weekday() + 1) % 7, week, local.hour, local.minute)


def rule_date(year, rule):
    """The local date-time a Windows rule names in @year"""
    month, dow, week, hour, minute = rule
    first = dt.date(year, month, 1)
    day = 1 + (dow - (first.weekday() + 1) % 7) % 7 + (week - 1) * 7
    while day > calendar.monthrange(year, month)[1]:
        day -= 7
    return dt.datetime(year, month, day) + dt.timedelta(hours=hour, minutes=minute)


def zone_rule(name, year):
    """(bias, daylight bias, standard date, daylight date) or None if the
    zone's rule cannot be said in Windows' form"""
    zone = zoneinfo.ZoneInfo(name)
    tr = transitions(zone, year)
    if len(tr) != 2:
        off = dt.datetime(year, 12, 31, 12, tzinfo=UTC).astimezone(zone).utcoffset()
        return (-minutes(off), 0, None, None), len(tr) == 0
    std = min(tr[0][1], tr[0][2])
    dst = max(tr[0][1], tr[0][2])
    start = next(t for t in tr if t[2] == dst)  # into daylight time
    stop = next(t for t in tr if t[2] == std)   # back to standard time
    s_local = (start[0] + start[1]).replace(tzinfo=None)   # standard time
    e_local = (stop[0] + stop[1]).replace(tzinfo=None)     # daylight time
    best = None
    for s_last in (True, False):
        for e_last in (True, False):
            d_rule = windows_date(s_local, s_last and s_local.day + 7 > calendar.monthrange(year, s_local.month)[1])
            s_rule = windows_date(e_local, e_last and e_local.day + 7 > calendar.monthrange(year, e_local.month)[1])
            ok = True
            for y in range(year - 2, year + 4):
                t = transitions(zone, y)
                if len(t) != 2:
                    ok = False
                    break
                a = next(x for x in t if x[2] == dst)
                b = next(x for x in t if x[2] == std)
                if (a[0] + a[1]).replace(tzinfo=None) != rule_date(y, d_rule) or \
                   (b[0] + b[1]).replace(tzinfo=None) != rule_date(y, s_rule):
                    ok = False
                    break
            if ok:
                best = (s_rule, d_rule)
                break
        if best:
            break
    exact = best is not None
    if not best:
        best = (windows_date(e_local, False), windows_date(s_local, False))
    return (-minutes(std), -(minutes(dst) - minutes(std)), best[0], best[1]), exact


def city(zone):
    if zone.startswith('Etc/'):
        return None
    return zone.split('/')[-1].replace('_', ' ')


def display(key, bias, cities):
    off = -bias
    sign = '+' if off >= 0 else '-'
    head = '(UTC)' if off == 0 else '(UTC%s%02d:%02d)' % (sign, abs(off) // 60, abs(off) % 60)
    if key == 'UTC':
        what = 'Coordinated Universal Time'
    elif cities:
        what = ', '.join(cities)
    else:
        what = key.replace(' Standard Time', '')
    return '%s %s' % (head, what)


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cldr', help='windowsZones.xml (downloaded from CLDR_URL when left out)')
    ap.add_argument('--year', type=int, default=dt.date.today().year)
    ap.add_argument('--out', default=OUT)
    a = ap.parse_args()
    use_tzdata_package()

    data = open(a.cldr, 'rb').read() if a.cldr else urllib.request.urlopen(CLDR_URL).read()
    root = ET.fromstring(data)
    ver = root.find('.//windowsZones/mapTimezones')
    cldr_ver = '%s/%s' % (ver.get('otherVersion'), ver.get('typeVersion')) if ver is not None else '?'
    m = re.search(r'release-(\d+)', CLDR_URL)
    cldr_rel = 'CLDR %s' % m.group(1) if m and not a.cldr else 'CLDR'

    keys, main_zone, cities = [], {}, {}
    for mz in root.iter('mapZone'):
        key, terr, types = mz.get('other'), mz.get('territory'), mz.get('type').split()
        if key not in cities:
            keys.append(key)
            cities[key] = []
        if terr == '001':
            main_zone[key] = types[0]
        elif terr != 'ZZ':
            c = city(types[0])
            if c:
                rank = TERR_RANK.index(terr) if terr in TERR_RANK else len(TERR_RANK)
                cities[key].append((rank, c))

    rows = []
    for key in keys:
        zone = main_zone.get(key)
        if not zone:
            continue
        (bias, dbias, sdate, ddate), exact = zone_rule(zone, a.year)
        if not exact:
            print('note: %s (%s) keeps no single rule; using %d\'s' % (key, zone, a.year), file=sys.stderr)
        cs = [city(zone)] if city(zone) else []
        for _, c in sorted(cities[key]):
            if c not in cs and len(cs) < 3:
                cs.append(c)
        std = 'Coordinated Universal Time' if key == 'UTC' else key
        dlt = key.replace('Standard Time', 'Daylight Time') if sdate else std
        rows.append((bias, display(key, bias, cs), key, std, dlt, dbias, sdate, ddate))
    rows.sort(key=lambda r: (-r[0], r[1]))

    def sysdate(d):
        if not d:
            return '{ 0, 0, 0, 0, 0 }'
        return '{ %d, %d, %d, %d, %d }' % d

    lines = [
        '/* tzdata.inc — the time zones NovaOS offers (kernel/ke/timezone.c)',
        ' *',
        ' * GENERATED by tools/gen_timezones.py: do not edit; run it again.',
        ' * Zone names and cities: Unicode %s windowsZones (%s), Unicode License v3.' % (cldr_rel, cldr_ver),
        ' * Offsets and daylight-saving rules: IANA tz database %s (public domain),' % tzdata_version(),
        ' * the rules of %d in Windows\' form (month, weekday, week 1-5 where 5 is' % a.year,
        ' * the last, hour, minute; local time), ordered as Windows lists them.',
        ' */',
        '',
    ]
    for r in rows:
        bias, disp, key, std, dlt, dbias, sdate, ddate = r
        lines.append('    { %s, %s, %s, %s, %d, %d, %s, %s },' % (
            c_str(key), c_str(disp), c_str(std), c_str(dlt), bias, dbias, sysdate(sdate), sysdate(ddate)))
    with open(a.out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('%d zones -> %s' % (len(rows), os.path.relpath(a.out)))


if __name__ == '__main__':
    main()
