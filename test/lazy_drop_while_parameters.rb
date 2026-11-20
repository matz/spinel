rows = [[1, 2], [3, 4], [5, 6]]
p rows.lazy.drop_while { |left, right| right == 2 }.force
p rows.lazy.drop_while { |*args| args == [[1, 2]] }.force
p rows.lazy.drop_while { |left, *rest| rest == [2] }.first
p rows.lazy.drop_while { false }.first
p rows.lazy.drop_while { |(left, right)| right == 2 }.force
