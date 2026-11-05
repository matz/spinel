# A constant read that triggers an autoload of a file in another region runs
# that file's top level inside the statement, so it sees K's first v
class K; def v = 1; end
def probe = K.new.v
autoload :Lib, File.join(__dir__, "require_runs_region_autoload_lib")
Lib
class K; def v = 2; end
puts probe
