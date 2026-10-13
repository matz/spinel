# Dir.foreach, Dir.each_child and Dir.glob with a block answer nil. They were
# rewritten to `entries(...).each { }` and its kin, which answered the Array
# of entries: `p Dir.foreach(d) { }` printed it, and a method whose last
# expression is the walk -- tzinfo's enum_timezones changes each entry in
# place (`entry.encode!`) -- was refused as handing that String out again.
require "tmpdir"
dir = File.join(Dir.tmpdir, "sp_t_dir_nil_#{Process.pid}")
Dir.mkdir(dir) unless Dir.exist?(dir)
File.write("#{dir}/a", "")
File.write("#{dir}/b", "")
p(Dir.foreach(dir) { |e| })
p(Dir.each_child(dir) { |e| })
p(Dir.glob("#{dir}/*") { |e| })
def walk(d, seen)
  Dir.foreach(d) do |entry|
    entry.encode!(Encoding::UTF_8)
    seen << entry.upcase unless entry.start_with?(".")
  end
end
seen = []
p walk(dir, seen)
p seen.sort
File.delete("#{dir}/a")
File.delete("#{dir}/b")
Dir.rmdir(dir)
