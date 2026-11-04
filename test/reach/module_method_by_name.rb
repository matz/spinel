# Site.mkdir_p and Site.clean share their names' calls with FileUtils, whose
# methods go by name: a module holds nothing back (#10592). A call on the
# module of one of them (FileUtils.chdir's `cd`) cost it that, so the passes
# that type a body held FileUtils.mkdir_p and what it calls back, the last
# pass kept them, and FileUtils.chmod reached the C with its reads untyped:
# its boxed mode went into the sp_int slot of sp_file_chmod.
require "fileutils"

module Site
  def self.write(dir)
    mkdir_p(dir) if dir
  end

  def self.mkdir_p(dir)
  end

  def self.clean(path)
    Dir.rmdir(path) if path
  end
end

Dir.chdir("/")
Site.clean("/nonexistent-#{Process.pid}/x") rescue nil
puts "ok"
