# A "." or ".." component of a glob pattern keeps its spelling: Dir.glob
# took the name of the directory from the disk for it, and found nothing.
require "tmpdir"

dir = "#{Dir.tmpdir}/spinel_glob_dot_#{Process.pid}"
Dir.mkdir(dir)
Dir.mkdir("#{dir}/sub")
File.write("#{dir}/a", "")
File.write("#{dir}/sub/b", "")
short = ->(list) { list.map { |x| x.sub(dir, "D") } }
p short.(Dir.glob("#{dir}/./*"))
p short.(Dir.glob("#{dir}/sub/../*"))
p short.(Dir.glob("#{dir}/./sub/./b"))
p short.(Dir.glob("#{dir}/."))
p short.(Dir.glob("#{dir}/sub/.."))
p Dir.glob("./*", base: dir)
p Dir.glob("sub/../a", base: dir)
p Dir.glob("./sub/*", base: dir)
File.delete("#{dir}/sub/b")
File.delete("#{dir}/a")
Dir.rmdir("#{dir}/sub")
Dir.rmdir(dir)
