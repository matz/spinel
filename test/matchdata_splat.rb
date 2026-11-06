# `*md` is md.to_a: the whole match, then each group (nil for one that did
# not take part). It was pushed whole, so a destructuring read nil.
md = "{uri}".match(/\{([+#])?(\w+)\}/)
_, op, var = *md
p [op, var]
p [*md]
p [:head, *md, :tail]
none = "x".match(/y/)
p [*none]
boxed = md
boxed = 1 if ARGV.size > 5
p [*boxed]
_, a, b = [*"k=v".match(/(\w)=(\w)/)]
p [a, b]
