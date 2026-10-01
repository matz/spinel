# A Regexp passed to a writer reached through a boxed receiver kept its
# value: the boxing of a typed Regexp answered nil, so a program's attr
# writer stored nil and a real IO's sync= turned buffering off.
class Log
  attr_accessor :sync
end
class Other
  attr_accessor :sync
end
xs = [Log.new, Other.new]
xs.each { |x| x.sync = /a/; p x.sync }
f = File.open(File::NULL, "w")
f.sync = false
ys = [Log.new, f]
ys.each { |y| y.sync = /b/; p y.sync }
h = {}
k = [h, Log.new][0]
k[:r] = /c/
p h[:r]
