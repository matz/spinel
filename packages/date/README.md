# date

`require "date"`: Date and DateTime, from ruby/date's pure-Ruby
implementation, with the changes below.

- Source: <https://github.com/ruby/date/pull/155> (Feature #21264, "Replace C
  extension with pure Ruby implementation for Ruby >= 3.3")
- Commit: `fe084e5a338e` (2026-09-27), with its `8ebc031` ("Drop fractional
  day support for Date") reverted: a Date keeps the C extension's day
  fraction (`Date.new(2001, 2, 3) + 0.5`, `Date.jd(2451944.5)`), as CRuby's
  answers. The difference is reported upstream (ruby/date#155, 2026-11-10).
- Files: `lib/date.rb` is `date.rb` here, `lib/date/*.rb` is `date/*.rb`
- License: Ruby's (`COPYING`) or the 2-clause BSD license (`BSDL`), both
  copied from that commit

Changes from that commit, each answering as before, so the compiler can
type and share what the code holds (to be offered upstream):

- a Date's Julian day, cutover and DateTime's time fields are stored as
  `Integer(...)` / `Float(...)` / `Rational(...)` of what they are given;
- `initialize_copy` reads the copied fields through protected readers, and
  `new_start`, `step`, `upto`, `downto` and the other constructors that set
  an allocated Date's day and cutover do it through a protected setter, not
  `instance_variable_get` / `instance_variable_set`;
- `sp_complete_frags` asks today's fields by name in a `case`, not by
  `__send__(sym)`;
- each `StringScanner` scans a copy of its String, and a parsed zone is
  read as `String(...)`;
- `deconstruct_keys`' `each` block ends in `nil`;
- `+` and `-` are Date's for DateTime too, as CRuby's one method serves both
  classes: they come down to private `_plus_days` / `_plus_fraction` hooks
  DateTime defines for itself, and `Date - Date` is `ajd - other.ajd` for
  both (a DateTime's offset counts, as in the C extension). An alias a
  program takes of `+` in Date (activesupport's `plus_without_duration`) is
  then the method a DateTime runs as well.

The changes that fix that commit's answers, to be offered upstream:

- `Date._parse(str, false)` drops its century marker (`_comp`) from the
  hash, as the C extension does;
- upstream's `to_string_value` (the parsers' and `strptime`'s StringValue)
  names what a `to_str` gave (`can't convert X to String (X#to_str gives
  Integer)`), names an anonymous class as `#<Class:0x...>`, and serves
  `strftime` too, which upstream still reads through a public `to_str`.

`date/constants.rb`, `date/core.rb` and `date/datetime.rb` lose their
`# encoding: US-ASCII` line: Spinel reads every source file as UTF-8 and
warns that another encoding's comment is ignored, so it warned in every
program that required date. The files are ASCII, so their bytes read the
same; their Strings (the month and day names, `inspect`) report UTF-8 where
CRuby's report US-ASCII. Upstream's `test_enc` and `test_inspect` check
that encoding; every other upstream test passes on these files under CRuby
(147 tests, test/date).

When the pull request is merged upstream, replace these files with the
released gem's `lib/`, keep the changes upstream has not taken, and update
the commit above.

The tests in `test/` run on demand, not in the gate:
`make pkg-ondemand-test`.
