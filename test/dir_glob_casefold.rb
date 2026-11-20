# Dir.glob with File::FNM_CASEFOLD: the pattern matches the names without
# regard to case, in a wildcard component, a literal one and a recursive
# walk, as a flag or as flags:; with FNM_DOTMATCH beside it the hidden
# names come too. (A glob with no flag follows the filesystem's case
# rule, so these cases all set the flag.)
require "tmpdir"
Dir.mktmpdir("sp_glob_casefold") do |d|
  Dir.chdir(d) do
    File.write("a.txt", "")
    File.write("B.TXT", "")
    File.write(".hid", "")
    Dir.mkdir("Sub")
    File.write("Sub/c.TXT", "")
    p Dir.glob("*.txt", File::FNM_CASEFOLD).sort
    p Dir.glob("*.TXT", flags: File::FNM_CASEFOLD).sort
    p Dir.glob("b*", File::FNM_CASEFOLD)
    p Dir.glob("[a-b].txt", File::FNM_CASEFOLD).sort
    p Dir.glob("sub/*.txt", File::FNM_CASEFOLD)
    p Dir.glob("SUB", File::FNM_CASEFOLD)
    p Dir.glob("**/*.txt", File::FNM_CASEFOLD).sort
    p Dir.glob("*", File::FNM_DOTMATCH | File::FNM_CASEFOLD).sort
    p Dir.glob("a.TXT", base: d, flags: File::FNM_CASEFOLD)
    p Dir.glob("*.txt", File::FNM_CASEFOLD, sort: false).sort
    p Dir.glob("nope*", File::FNM_CASEFOLD)
  end
end
