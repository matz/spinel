# spinel: share
# spinel: gc-stress
# Dir.glob takes the flags of File.fnmatch: FNM_DOTMATCH shows the hidden
# names and FNM_NOESCAPE makes a backslash a plain character, as a flag or
# as flags:, with base: and with sort: false; FNM_CASEFOLD, FNM_EXTGLOB and
# FNM_PATHNAME change nothing, as CRuby ignores them in a glob (the case
# rule is the filesystem's). The cases compare a glob with each flag to
# one without, so the answers hold on any filesystem.
require "tmpdir"
Dir.mktmpdir("sp_glob_flags") do |d|
  Dir.chdir(d) do
    File.write("a.txt", "")
    File.write("b.txt", "")
    File.write(".hid", "")
    File.write("a*b", "")
    File.write("a\\b", "")
    Dir.mkdir("sub")
    File.write("sub/c.txt", "")
    p Dir.glob("*.txt", File::FNM_CASEFOLD) == Dir.glob("*.txt")
    p Dir.glob("B*", flags: File::FNM_CASEFOLD) == Dir.glob("B*")
    p Dir.glob("sub/*", File::FNM_CASEFOLD | File::FNM_EXTGLOB | File::FNM_PATHNAME) == Dir.glob("sub/*")
    p Dir.glob("**/*.txt", File::FNM_CASEFOLD).sort
    p Dir.glob("a\\*b"), Dir.glob("a\\*b", File::FNM_NOESCAPE), Dir.glob("a\\*b", flags: File::FNM_NOESCAPE | File::FNM_CASEFOLD)
    p Dir.glob("*", File::FNM_DOTMATCH).sort, Dir.glob("*", File::FNM_DOTMATCH | File::FNM_CASEFOLD).sort
    p Dir.glob("*.txt", base: d, flags: File::FNM_CASEFOLD).sort, Dir.glob("*.txt", File::FNM_CASEFOLD, sort: false).sort
    p Dir.glob("nope*", File::FNM_CASEFOLD)
    # Brace alternatives are expanded with or without FNM_EXTGLOB.
    p Dir.glob("{a,b}.txt", File::FNM_EXTGLOB)
    p Dir.glob("{a,b}.txt", flags: File::FNM_EXTGLOB, base: d, sort: false).sort
    p Dir.glob("*", 4.9).sort
    p Dir.glob("*", flags: 4.9, base: d).sort
    ["x", nil, true, false].each do |flags|
      begin
        Dir.glob("*", flags)
      rescue TypeError => e
        p e.message
      end
      begin
        Dir.glob("*", flags: flags, base: d)
      rescue TypeError => e
        p e.message
      end
    end
  end
end
