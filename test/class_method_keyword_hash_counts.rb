# A class method that takes no keywords counts a keyword hash as one more
# positional, as CRuby does, also where it takes optional positionals
def t(l)
  r = yield
  puts "#{l}: #{r.inspect}"
rescue => e
  puts "#{l}: #{e.class}: #{e.message}"
end
t("basename 3") { File.basename("/a/b.rb", ".rb", a: 1) }
t("basename 2") { File.basename("/a/b.rb", a: 1) }
t("basename") { File.basename("/a/b.rb", ".rb") }
t("dirname 3") { File.dirname("/a/b", 1, a: 1) }
t("expand_path 3") { File.expand_path("a", "/b", a: 1) }
t("absolute_path 3") { File.absolute_path("a", "/b", a: 1) }
t("log 3") { Math.log(8, 2, a: 1) }
t("log") { Math.log(8, 2) }
t("rand 2") { Random.rand(1, a: 1) }
