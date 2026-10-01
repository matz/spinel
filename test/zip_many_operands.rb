# zip with more than 16 operands: the typed arms held 16 and dropped the
# rest without a word, so the tuples came out short.

# a literal Integer receiver, 17 operands
p [0].zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], [17])

# exactly 16 still takes the typed arm
p [0].zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16]).first.size

# String and Float receivers; the last operand is the one that went missing
p ["s"].zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], ["last"]).first.last
p [0.5].zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], [17.5]).first.last

# a boxed receiver read out of a container
x = [[0, 1], "s"]
p x[0].zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], [17, 18])

# shorter operands pad with nil, past the 16th too
p [0, 1].zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], [17], [18, 19]).last

# a Range and a Hash receiver
p (0..0).zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], [17]).first.size
p({a: 1}.zip([1], [2], [3], [4], [5], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], [17]).first.last)

# twenty operands of mixed types
r = [1].zip([2], ["3"], [4.0], [nil], [:five], [6], [7], [8], [9], [10], [11], [12], [13], [14], [15], [16], [17], [18], [19], [20])
p r.first.size
p r.first[-4, 4]
