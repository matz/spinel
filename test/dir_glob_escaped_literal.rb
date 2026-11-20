# Dir.glob: a backslash escapes the character after it in a component with
# no wildcard too, so "foo\ bar/*" finds the files of "foo bar", as CRuby
# does; the names answered are the names on the disk.
require "tmpdir"
Dir.mktmpdir("sp_glob_escape") do |d|
  Dir.chdir(d) do
    Dir.mkdir("foo bar")
    File.write("foo bar/file1.txt", "")
    File.write("foo bar/file2.txt", "")
    File.write("a*b", "")
    File.write("a\\b", "")
    Dir.mkdir("sub dir")
    Dir.mkdir("sub dir/deep")
    File.write("sub dir/deep/x.rb", "")
    p Dir.glob("foo\\ bar/*").sort
    p Dir.glob("foo\\ bar/file1.txt")
    p Dir.glob("foo\\ bar")
    p Dir.glob("foo\\ b*/*").sort, Dir.glob("f*\\ bar/*").sort
    p Dir.glob("a\\*b"), Dir.glob("a\\\\b")
    p Dir.glob("sub\\ dir/**/*.rb"), Dir.glob("sub\\ dir/deep/x.rb")
    p Dir.glob("foo\\ bar/*", base: d).sort, Dir.glob("foo\\ bar/*", File::FNM_DOTMATCH).sort
    p Dir.glob("foo\\ baz/*"), Dir.glob("foo\\\\ bar/*")
  end
end
