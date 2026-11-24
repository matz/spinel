# spinel: share
# spinel: gc-stress
require "pathname"
require "tmpdir"
dir = "#{Dir.tmpdir}/spinel_pathname_glob_flags_#{Process.pid}"
begin
  Dir.mkdir(dir)
  Dir.mkdir("#{dir}/sub")
  File.write("#{dir}/visible", "")
  File.write("#{dir}/.hidden", "")
  File.write("#{dir}/sub/.inner", "")
  short = ->(list) { list.map { |x| x.to_s.sub(dir, "D") } }
  # the class form
  p short.(Pathname.glob("#{dir}/*", File::FNM_DOTMATCH))
  p short.(Pathname.glob("#{dir}/*", 0))
  p Pathname.glob("#{dir}/*", File::FNM_DOTMATCH).map(&:class).uniq
  # the instance form: the receiver's own "." entry answers the receiver
  pn = Pathname.new(dir)
  p short.(pn.glob("*", File::FNM_DOTMATCH))
  p short.(Pathname.new("#{dir}/").glob("*", File::FNM_DOTMATCH))
  p short.(pn.glob("sub/*", File::FNM_DOTMATCH))
  # "./" in the pattern folds as #+ folds it (CRuby: self + match)
  p short.(pn.glob("./*", File::FNM_DOTMATCH))
  p short.(pn.glob("sub/./*", File::FNM_DOTMATCH))
  p short.(pn.glob("*", 0))
  # flags held in a variable
  [0, File::FNM_DOTMATCH].each do |flags|
    p short.(Pathname.glob("#{dir}/*", flags))
    p short.(pn.glob("*", flags))
  end
  Dir.chdir(dir) { p Pathname.new(".").glob("*", File::FNM_DOTMATCH).map(&:to_s) }
  # All flag words reach Dir.glob, including combinations. CASEFOLD follows
  # the filesystem, so compare with the unflagged result on APFS and ext4.
  [File::FNM_CASEFOLD, File::FNM_PATHNAME, File::FNM_EXTGLOB].each do |flags|
    p Pathname.glob("#{dir}/V*", flags) == Pathname.glob("#{dir}/V*")
    p pn.glob("V*", flags) == pn.glob("V*")
    p short.(pn.glob("{visible,sub}", flags)).sort
  end
  File.write("#{dir}/a*b", "")
  File.write("#{dir}/a\\b", "")
  p short.(Pathname.glob("#{dir}/a\\*b", File::FNM_NOESCAPE))
  p short.(pn.glob("a\\*b", File::FNM_NOESCAPE | File::FNM_CASEFOLD))
  File.delete("#{dir}/a*b")
  File.delete("#{dir}/a\\b")
  p short.(Pathname.glob("#{dir}/**/*", File::FNM_DOTMATCH | File::FNM_EXTGLOB))
  p short.(pn.glob("**/*", File::FNM_DOTMATCH | File::FNM_PATHNAME))
  # a Float is truncated
  p short.(pn.glob("*", 4.9))
  # a "." pattern names the receiver
  p short.(pn.glob("."))
  # flags that are not a number
  ["x", nil].each do |flags|
    begin
      Pathname.glob("#{dir}/*", flags)
    rescue TypeError => e
      p [:class, e.message]
    end
    begin
      pn.glob("*", flags)
    rescue TypeError => e
      p [:instance, e.message]
    end
  end
ensure
  File.delete("#{dir}/sub/.inner") if File.exist?("#{dir}/sub/.inner")
  Dir.rmdir("#{dir}/sub") if Dir.exist?("#{dir}/sub")
  File.delete("#{dir}/visible") if File.exist?("#{dir}/visible")
  File.delete("#{dir}/.hidden") if File.exist?("#{dir}/.hidden")
  Dir.rmdir(dir) if Dir.exist?(dir)
end
