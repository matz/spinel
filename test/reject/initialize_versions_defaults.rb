# Opt's initialize is defined in two files, and the load order leaves which
# one the class ends with to run time (first.rb is required under a
# condition and again from again.rb). The constructor would run the
# entry over both, but each version has a default of its own for `b`,
# which the constructor's fixed parameters cannot leave out: refused,
# where it would run first.rb's (CRuby prints [10, 3]).
require_relative "initialize_versions_defaults/first" if ARGV.empty?
require_relative "initialize_versions_defaults/again"
require_relative "initialize_versions_defaults/last"
p Opt.new(1).v
