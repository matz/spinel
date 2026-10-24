# spinel: gc-stress
require "tmpdir"
dir = File.join(Dir.tmpdir, "sp_glob_keywords_#{Process.pid}")
Dir.mkdir(dir)
begin
  File.write(File.join(dir, "z.md"), "z")
  File.write(File.join(dir, "a.md"), "a")
  File.write(File.join(dir, ".hidden.md"), "h")
  p Dir.glob("a.md", base: dir)
  p Dir.glob("*.md", base: dir, flags: File::FNM_DOTMATCH)
  p Dir.glob("*.md", flags: File::FNM_DOTMATCH, base: dir, sort: true)
  # sort: false answers the walk's order, which the filesystem picks
  p Dir.glob("*.md", base: dir, sort: false).sort
  p Dir.glob("*.md", sort: false, base: dir, flags: File::FNM_DOTMATCH).sort
  flags = File::FNM_DOTMATCH
  p Dir.glob("*.md", base: dir, flags: flags)
  p Dir.glob("*.md", base: dir, flags: 0)
  p Dir.glob("*.md", base: dir, flags: 4.0)
  begin
    Dir.glob("*.md", base: dir, sort: nil)
  rescue ArgumentError => e
    p e.message
  end
  begin
    Dir.glob("*.md", base: dir, flags: nil)
  rescue TypeError => e
    p e.message
  end
  p Dir.glob("*.md", base: dir)
ensure
  File.delete(File.join(dir, "z.md"))
  File.delete(File.join(dir, "a.md"))
  File.delete(File.join(dir, ".hidden.md"))
  Dir.rmdir(dir)
end
