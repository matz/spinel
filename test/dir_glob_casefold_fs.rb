# Dir.glob on a filesystem that does not tell letter cases apart (APFS and
# HFS+ as macOS makes them) matches a name in any case and answers the
# name as the disk spells it, as CRuby does, which asks each directory it
# opens whether its volume tells cases apart. On one that does (Linux), a
# pattern matches its case alone. The cases print the same on both: the
# filesystem's rule is read from a stat of the other case.
require "tmpdir"
Dir.mktmpdir("sp_glob_case") do |d|
  Dir.chdir(d) do
    File.write("a.txt", "")
    File.write("B.TXT", "")
    Dir.mkdir("sub")
    File.write("sub/c.txt", "")
    File.write("sub/D.txt", "")
    folds = File.exist?("A.TXT")
    p folds == File.exist?("SUB/C.TXT")
    # a literal name in another case: the name on the disk
    p Dir.glob("A.txt") == (folds ? ["a.txt"] : [])
    p Dir.glob("b.txt") == (folds ? ["B.TXT"] : [])
    p Dir.glob("a.txt") == ["a.txt"]
    # a wildcard component
    p Dir.glob("*.TXT").sort == (folds ? ["B.TXT", "a.txt"] : ["B.TXT"])
    p Dir.glob("*.txt").sort == (folds ? ["B.TXT", "a.txt"] : ["a.txt"])
    p Dir.glob("[a-b].txt").sort == (folds ? ["B.TXT", "a.txt"] : ["a.txt"])
    p Dir.glob("?.TXT").sort == (folds ? ["B.TXT", "a.txt"] : ["B.TXT"])
    # a directory component in another case, literal and with a wildcard
    p Dir.glob("SUB/*.txt").sort == (folds ? ["sub/D.txt", "sub/c.txt"] : [])
    p Dir.glob("sub/*.txt").sort
    p Dir.glob("S*/C.TXT") == (folds ? ["sub/c.txt"] : [])
    p Dir.glob("**/*.TXT").sort == (folds ? ["B.TXT", "a.txt", "sub/D.txt", "sub/c.txt"] : ["B.TXT"])
    p Dir.glob("{A,b}.txt").sort == (folds ? ["B.TXT", "a.txt"] : [])
    p Dir.glob("A.txt", base: d) == (folds ? ["a.txt"] : [])
    p Dir.glob(File.join(d, "A.txt")) == (folds ? [File.join(d, "a.txt")] : [])
    p Dir["A.TXT", "sub/d.TXT"] == (folds ? ["a.txt", "sub/D.txt"] : [])
    # a name that is in no case
    p Dir.glob("Z.txt"), Dir.glob("*.rb")
    # a letter past ASCII: the volume folds it for a literal name (the disk's
    # spelling comes back), the matcher of a wildcard folds the ASCII letters
    # alone, as CRuby's does
    Dir.mkdir("u")
    File.write("u/\u00C9.txt", "")
    p Dir.glob("u/\u00E9.txt") == (folds ? ["u/\u00C9.txt"] : []), Dir.glob("u/\u00C9.txt"), Dir.glob("u/\u00E9*"), Dir.glob("u/*.TXT") == (folds ? ["u/\u00C9.txt"] : [])
    # a bracket of one member under the fold takes the character after the
    # member, as CRuby's matcher does: "[a]" takes "]" and not "A"
    Dir.mkdir("br")
    File.write("br/a.txt", "")
    File.write("br/].txt", "")
    p Dir.glob("br/[a].txt").sort == (folds ? ["br/].txt", "br/a.txt"] : ["br/a.txt"]), Dir.glob("br/[A].txt") == (folds ? ["br/].txt"] : [])
    # File.fnmatch keeps its own rule: the cases of the pattern, unless asked
    p File.fnmatch("A.txt", "a.txt"), File.fnmatch("A.txt", "a.txt", File::FNM_CASEFOLD)
  end
end
