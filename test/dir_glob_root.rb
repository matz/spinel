# A pattern of only separators names the root: Dir.glob("/") returned [].
require "tmpdir"

p Dir.glob("/")
p Dir.glob("//")
p Dir["/"]
p Dir.glob(["/"])
p Dir.glob("/", base: Dir.tmpdir)
p Dir.glob("/", File::FNM_DOTMATCH)
p Dir.glob("")

dir = "#{Dir.tmpdir}/spinel_glob_root_#{Process.pid}"
Dir.mkdir(dir)
p Dir.glob("#{dir}/") == ["#{dir}/"]
Dir.rmdir(dir)

# Dir.glob returns [] for a pattern of 2048 bytes or more.
p Dir.glob("/" * 2047 + "spinel_no_such_entry")
p Dir.glob("/" * 5000)
