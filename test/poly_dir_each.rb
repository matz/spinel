# spinel: gc-stress
require "tmpdir"
path = File.join(Dir.tmpdir, "spinel-boxed-dir-each-#{Process.pid}")
Dir.mkdir(path)
File.write(File.join(path, "a"), "")
d = Dir.open(path)
begin
  x = [d, 0][0]
  seen = []
  x.each do |entry|
    seen << entry
    break
  end
  p seen.size
  p d.read.nil?
  d.read
  all = []
  r = x.each { |entry| all << entry }
  p all.sort
  p r.equal?(d)
  p(x.each { |entry| break entry.size if entry == "a" })
  # a block without parameters still steps the walk through every entry
  n = 0
  x.each { n += 1 }
  p n
  g = [Enumerator.new { |y| y << 1; y << 2; y << 3 }, 0][0]
  steps = 0
  g.each { steps += 1 }
  p steps
ensure
  d.close
  File.delete(File.join(path, "a"))
  Dir.rmdir(path)
end
