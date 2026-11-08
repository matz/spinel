# net

`require "net/http"` and `require "net/https"`: an HTTP/HTTPS client written
for Spinel; see the top of `net/http.rb` for what it leaves out.

`Net::HTTPResponse.read_new(buffered_io)` reads one response head and selects
the response class by status code. It folds continuation lines and preserves
repeated header values; entity bytes stay in the caller's `Net::BufferedIO`.
The parser follows net-http 0.9.1 and uses net-protocol 0.2.2's positional
`readuntil(terminator, ignore_eof = false)` API.

`require "net/smtp"`: Net::SMTP, ruby/net-smtp's SMTP client (a bundled gem
since Ruby 3.1), over `net/protocol`, ruby/net-protocol's base for Ruby's
network clients (a default gem). Both are vendored from the versions CRuby 4.0
ships:

- net-smtp: <https://github.com/ruby/net-smtp>, release v0.5.1, commit
  `a0075eb8a74910bd05566f0a208589263bce7597` (2025-02-05). `lib/net/smtp.rb`
  is `net/smtp.rb` here, `lib/net/smtp/*.rb` is `net/smtp/*.rb`.
- net-protocol: <https://github.com/ruby/net-protocol>, version 0.2.2 at
  commit `6c5734dc1e0087782c00b6d0b5f8d0915362bf7f` (2025-11-09, the v0.2.2
  release plus documentation comments). `lib/net/protocol.rb` is
  `net/protocol.rb` here.
- License: Ruby's (`COPYING`) or the 2-clause BSD license (`BSDL`), both
  copied from those commits (the two gems ship the same files).

They are unchanged except for two hunks:

- At the end of `net/smtp.rb`, upstream loads the authenticators with
  `Dir.glob("#{__dir__}/smtp/auth_*.rb")` and a `require_relative` of each
  match, which a compiled program cannot do, since every require is resolved
  at compile time. Here the four are required by name, in the order the glob
  gives them.
- In `net/smtp/auth_cram_md5.rb`, upstream picks `OpenSSL::Digest` or
  `::Digest` at run time and names `MD5` under it (`digest_class::MD5`).
  Both define an `MD5`, and Spinel resolves a constant under a receiver
  known only at run time by its name alone, so here the method picks the MD5
  class itself (`md5_class`).

To update, replace these files with a newer release's `lib/`, carry the hunks
above over, and update the commits.

The `net_smtp_*` tests in `test/` run on demand, not in the gate:
`make pkg-ondemand-test`. They talk to a plain-text server, so STARTTLS and
SMTPS, which go through the `openssl` package, are not covered.
