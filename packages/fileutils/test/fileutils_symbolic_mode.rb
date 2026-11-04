require "fileutils"
require "tmpdir"

def mode_of(path) = (File.stat(path).mode & 0o7777).to_s(8)

dir = Dir.mktmpdir("fu-symbolic")
f = "#{dir}/f"
File.write(f, "x")

# Integer and String modes through one call
File.chmod(0o640, f)
[0o644, "u+x", "g-r", "o=rw", "a-w", "u=rw,go=r", "+x", "a=", "u=rwx,g=u,o=g-w", "u+r-w", "g=u+x"].each do |m|
  FileUtils.chmod(m, f)
  puts "#{m.is_a?(Integer) ? m.to_s(8) : m} #{mode_of(f)}"
end

FileUtils.chmod(0o644, f)

# X is x on a directory only
d = "#{dir}/d"
Dir.mkdir(d)
File.write("#{d}/in", "x")
File.chmod(0o600, "#{d}/in")
File.chmod(0o600, d)
FileUtils.chmod("u+X", d)
FileUtils.chmod("u+X", "#{d}/in")
puts mode_of(d), mode_of("#{d}/in")

# each path reads its own mode
g = "#{dir}/g"
File.write(g, "x")
File.chmod(0o600, g)
FileUtils.chmod("g+r", [f, g])
puts mode_of(f), mode_of(g)

# chmod_R reads each entry's mode, the X of each kind
File.chmod(0o700, d)
FileUtils.chmod_R("go+rX", d)
puts mode_of(d), mode_of("#{d}/in")

FileUtils.chmod("u+x", f, noop: true)
puts mode_of(f)
FileUtils.chmod("u+x", "x", verbose: true, noop: true)

# install reads the mode against the source
File.chmod(0o600, g)
FileUtils.install(g, "#{dir}/h", mode: "g+r")
puts mode_of("#{dir}/h")

["u+q", "z+r", "u"].each do |m|
  begin
    FileUtils.chmod(m, f)
  rescue ArgumentError => e
    puts e.message
  end
end

begin
  File.chmod("u+x", f)
rescue TypeError => e
  puts e.message
end

FileUtils.rm_rf(dir)
