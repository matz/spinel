# date

`require "date"`: Date and DateTime, from ruby/date's pure-Ruby
implementation, with the changes below.

- Source: <https://github.com/ruby/date/pull/155> (Feature #21264, "Replace C
  extension with pure Ruby implementation for Ruby >= 3.3")
- Commit: `8795e36106c80dceb44650424c5fadbbc0436c35` (2026-06-30)
- Files: `lib/date.rb` is `date.rb` here, `lib/date/*.rb` is `date/*.rb`
- License: Ruby's (`COPYING`) or the 2-clause BSD license (`BSDL`), both
  copied from that commit

Changes from that commit, each answering as before, so the compiler can
type and share what the code holds (to be offered upstream):

- a Date's Julian day, cutover and DateTime's time fields are stored as
  `Integer(...)` / `Float(...)` / `Rational(...)` of what they are given;
- `initialize_copy` reads the copied fields through protected readers, and
  `new_start`, `step`, `upto` and `downto` set them through a protected
  setter, not `instance_variable_get` / `instance_variable_set`;
- `sp_complete_frags` asks today's fields by name in a `case`, not by
  `__send__(sym)`;
- each `StringScanner` scans a copy of its String, and a parsed zone is
  read as `String(...)`;
- `deconstruct_keys`' `each` block ends in `nil`;
- `+` and `-` are Date's for DateTime too, as CRuby's one method serves both
  classes: they come down to private `_plus_days` / `_plus_fraction` hooks
  DateTime defines for itself, and `Date - Date` is `ajd - other.ajd` for
  both (the Date form read the day fractions and dropped a DateTime's
  offset). An alias a program takes of `+` in Date (activesupport's
  `plus_without_duration`) is then the method a DateTime runs as well, where
  a `DateTime#+` of its own was another method the alias settling refused.

One change fixes that commit's answer, also to be offered upstream: the
parsers, `strptime` and `strftime` read a non-String argument through a
private `string_value`, as CRuby's StringValue does (`to_str` if it has one,
else a TypeError naming `nil`, `true`, `false` or the class).

When the pull request is merged upstream, replace these files with the
released gem's `lib/`, keep the changes upstream has not taken, and update
the commit above.

The tests in `test/` run on demand, not in the gate:
`make pkg-ondemand-test`.
