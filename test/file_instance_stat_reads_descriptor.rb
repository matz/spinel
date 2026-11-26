# File#stat, #size, #mtime, #atime, #ctime, #chmod and #truncate act on
# the open file (its descriptor), not on whatever its path names now;
# File#lstat still reads the path. A File::Stat is a copy taken when it
# was made, so it outlives the stream and later changes.
require "tmpdir"

dir = Dir.mktmpdir("file-fstat")
path = File.join(dir, "f")
moved = path + ".moved"
File.write(path, "old contents")
File.utime(Time.at(1_000_000_000), Time.at(1_000_000_000), path)
a = File.open(path, "r+")
old_ino = File.stat(path).ino
File.rename(path, moved)
File.write(path, "new")
File.utime(Time.at(1_500_000_000), Time.at(1_500_000_000), path)

s = a.stat
puts "stat names the #{s.ino == old_ino ? 'old' : 'new'} file"
puts "stat size #{s.size} file? #{s.file?} ftype #{s.ftype}"
puts "stat == File.stat(moved) #{s == File.stat(moved)}"
puts "stat mtime #{s.mtime.to_i}"
puts "size #{a.size}"
puts "mtime #{a.mtime.to_i} atime #{a.atime.to_i}"
puts "ctime of the open file #{a.ctime == File.ctime(moved)}"
puts "lstat reads the path, size #{a.lstat.size}"

a.chmod(0o600)
puts "chmod old #{format('%o', File.stat(moved).mode & 0o777)} new #{format('%o', File.stat(path).mode & 0o777)}"
a.truncate(3)
puts "truncate old #{File.size(moved)} new #{File.size(path)}"
puts "the earlier stat keeps its size #{s.size}"
a.seek(0, IO::SEEK_END)
a.write("xy")
puts "size counts a pending write #{a.size}"

a.close
puts "stat after close names the #{s.ino == old_ino ? 'old' : 'new'} file, size #{s.size}"
%w[stat size mtime chmod].each do |m|
  begin
    m == "chmod" ? a.chmod(0o644) : a.send(m)
    puts "#{m} on a closed File: no error"
  rescue IOError => e
    puts "#{m} on a closed File: #{e.class} #{e.message}"
  end
end

r, w = IO.pipe
puts "pipe stat pipe? #{r.stat.pipe?}"
r.close
w.close

File.delete(path)
File.delete(moved)
Dir.rmdir(dir)
