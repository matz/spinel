# A Symbol a block parameter holds, passed on as `&s`: the parameter is an
# element of the Array the iterator runs over.
syms = [:upcase, :size]
p syms.map { |s| ["ab", "c"].map(&s) }

[:reverse, :length].each { |s| p %w[xy zzz].map(&s) }

%i[upcase capitalize].each_with_index { |s, i| p [i, %w[ab cd].map(&s)] }

p [:even?, :odd?].flat_map { |s| [1, 2, 3].select(&s) }

# a builtin that never calls its block takes the Symbol as it is
fs = [proc { }, :to_s]
h = {}
h.store(1, 2, &fs[1])
a = [0]
a.push(1, &fs[1])
p h, a

# each_with_object passes the element as the first parameter
p [:upcase, :size].each_with_object([]) { |s, out| out << ["ab"].map(&s) }
