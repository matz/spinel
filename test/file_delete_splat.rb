# File.delete and File.unlink with a lone splat delete every file in the
# Array and answer how many, as with the paths written out.
fs = (1..3).map { |i| "/tmp/spinel_del_splat_#{Process.pid}_#{i}" }
fs.each { |f| File.write(f, "x") }
p File.delete(*fs)
p fs.map { |f| File.exist?(f) }
gs = fs.first(2)
gs.each { |f| File.write(f, "y") }
p File.unlink(*gs)
p gs.any? { |f| File.exist?(f) }
File.write(fs[0], "z")
begin
  File.delete(*[fs[0], fs[1]])
rescue SystemCallError => e
  p e.class
end
p File.exist?(fs[0])
# a missing file in the middle: the one before it is deleted, then it raises
hs = (1..3).map { |i| "/tmp/spinel_del_splat_#{Process.pid}_h#{i}" }
File.write(hs[0], "a")
File.write(hs[2], "c")
begin
  File.delete(*hs)
rescue SystemCallError => e
  p e.class
end
p hs.map { |f| File.exist?(f) }
File.delete(hs[2])

class LastDeletePath
  def initialize(first_path, last_path, replacement)
    @first_path = first_path
    @last_path = last_path
    @replacement = replacement
  end

  def to_path
    p File.exist?(@first_path)
    @first_path.replace(@replacement)
    @last_path
  end
end

first_path = "/tmp/spinel_del_splat_#{Process.pid}_first"
replacement = "#{first_path}_a_longer_filename_from_the_later_conversion"
last_path = "/tmp/spinel_del_splat_#{Process.pid}_last"
File.write(first_path, "")
File.write(replacement, "")
File.write(last_path, "")
mutable_path = first_path.dup
p File.delete(*[mutable_path, LastDeletePath.new(mutable_path, last_path, replacement)])
p [File.exist?(first_path), File.exist?(replacement), File.exist?(last_path)]
File.delete(first_path) if File.exist?(first_path)

File.write(first_path, "")
begin
  File.delete(*[first_path, replacement + "\0suffix",
                LastDeletePath.new(first_path.dup, last_path, replacement)])
rescue ArgumentError => e
  p e.message
end
p [File.exist?(first_path), File.exist?(replacement)]
File.delete(first_path) if File.exist?(first_path)
File.delete(replacement) if File.exist?(replacement)

# another File method keeps its splat as before
parts = %w[a b c]
p File.join(*parts)
