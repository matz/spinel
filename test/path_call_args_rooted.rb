# spinel: gc-stress
# A call whose earlier argument builds a new value (an interpolation,
# File.join) and whose later argument allocates held the earlier one only in
# the C argument list: under SPINEL_GC_STRESS=2 the later one's allocation
# freed it. A merge of String-keyed Hashes of two value layouts, both
# promoted to a new Hash by the call, did the same, and so did a merge
# answer merged again with a Hash the call promotes.
require "tmpdir"
dir = File.join(Dir.tmpdir, "spinel_pca_#{Process.pid}")
Dir.mkdir(dir)
n = 1
File.write(File.join(dir, "a#{n}.txt"), "hello")
p IO.copy_stream("#{dir}/a#{n}.txt", "#{dir}/b#{n}.txt")
p File.identical?("#{dir}/a#{n}.txt", "#{dir}/a#{n}.txt"), File.identical?("#{dir}/a#{n}.txt", "#{dir}/b#{n}.txt")
File.rename("#{dir}/b#{n}.txt", "#{dir}/c#{n}.txt")
p Dir.glob("*.txt", base: File.join(dir, "")).sort, Dir.glob("#{"*"}.txt", base: "#{dir}").sort
p File.expand_path("x#{n}", "/r#{n}"), File.join("#{dir}", "c#{n}.txt").size > 0
s1 = {"a" => 1, "b" => 2}
s2 = {"c" => "x", "d" => "y"}
s3 = s1.merge(s2)
p s3, s2.merge(s1)
p s1.merge(s2, {"e" => "#{n}w"}), s1.merge(s2).merge({"f" => "#{n}v"})
File.delete(File.join(dir, "a1.txt"), File.join(dir, "c1.txt"))
Dir.rmdir(dir)
x = 2
p ["#{x}a", "b#{x}"].join("#{x}-"), "#{x}".center(5, "#{x}*"), [1, 2].push("#{x}", "y#{x}").size
p "a#{x}b".sub("#{x}", "<#{x}>"), "a#{x}b".tr("#{x}", "z#{x}")
