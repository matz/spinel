# An Enumerator, and a boxed Range, Array or Hash, answer respond_to? for
# every name Enumerable gives them, also when the name is not a literal.
names = %i[tally chain lazy filter_map each_entry to_set slice_when
           slice_before slice_after chunk compact minmax_by uniq cycle
           length empty? zork]
e = [1, 2].each
p names.map { |n| e.respond_to?(n) }
xs = [[1, 2].each, (1..2), [1], { a: 1 }, "s"]
xs.each { |x| p names.map { |n| x.respond_to?(n) } }
# an Enumerator's own names
own = %i[next peek rewind size with_index next_values peek_values]
p own.map { |n| e.respond_to?(n) }
p own.map { |n| xs.first.respond_to?(n) }
# and the calls run
p xs[1].minmax_by { |v| -v }, xs[1].slice_when { |a, b| b > a }.to_a
p xs.first.tally, xs.first.filter_map { |v| v * 2 if v > 1 }
