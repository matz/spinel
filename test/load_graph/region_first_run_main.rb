# A file required from a method body loads when the method first runs, and
# the code it runs is the entry's at that statement: probe, which the file
# calls, sees K#v as it stands there (1), not as it stands once K reopens
# (2), and peek sees J#w as of then, though J's only instance is made by
# the file. An analysis that entered the file's region late would prove
# both calls bind the later version and print 2 and :w2 twice.
class K; def v = 1; end
class J; def w = :w1; end
def probe = K.new.v
def peek = $j ? $j.w : :none
def go = require_relative("region_first_run_lib")
go if ARGV.size == 0
class K; def v = 2; end
class J; def w = :w2; end
puts probe
p peek
