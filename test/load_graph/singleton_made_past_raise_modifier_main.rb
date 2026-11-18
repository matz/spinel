# A `raise ... if c` in an expression leaves the load when it runs, so the
# `class << M` after it in the same expression makes M's singleton class
# whenever the load goes on: the record answers that surely, and M.method on a
# name nothing defines raises NameError, not a refusal (lr_sg_when, which asks
# the climb that finds an extent sure).
module M; end
c = ARGV.size > 5
p((raise "q" if c; class << M; end))
begin
  M.method(:nope)
rescue NameError => e
  puts e.message
end
