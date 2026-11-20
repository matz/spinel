# Pathname's walks answer what CRuby's do: with a block, ascend, descend,
# each_filename and each_line yield and answer nil (each_child answers the
# children); without one, each answers an Enumerator over what it would
# yield. They answered the Array of those values either way (each_line,
# self).
require "pathname"
require "tmpdir"

pa = Pathname.new("a/b/c")
seen = []
p pa.ascend { |x| seen << x.to_s }, seen
seen = []
p pa.descend { |x| seen << x.to_s }, seen
seen = []
p pa.each_filename { |x| seen << x }, seen
p pa.ascend.class, pa.ascend.map(&:to_s)
p pa.descend.class, pa.descend.to_a.map(&:to_s)
p pa.each_filename.class, pa.each_filename.to_a
p Pathname.new("/x/y").descend.map(&:to_s), Pathname.new("/x/y").ascend.map(&:to_s)
p pa.ascend.first.to_s, pa.descend.first(2).map(&:to_s)

Dir.mktmpdir do |d|
  dir = Pathname.new(d)
  (dir + "one.txt").write("x\ny\n")
  (dir + "two.txt").write("z\n")
  lines = []
  p (dir + "one.txt").each_line { |l| lines << l }, lines
  p (dir + "one.txt").each_line.class, (dir + "one.txt").each_line.to_a
  names = []
  p dir.each_child(false) { |c| names << c.to_s }.map(&:to_s).sort, names.sort
  p dir.each_child.class, dir.each_child(false).map(&:to_s).sort
end
