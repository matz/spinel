# Splat (rest) target in multi-assignment.
# Covers leading / trailing / middle splat against array literals,
# different element types, and a rest-returning function source.

# Trailing splat from array literal (int_array)
a, *b = [1, 2, 3, 4]
puts a            # 1
puts b.length     # 3
b.each { |v| puts v }   # 2 / 3 / 4

# Leading splat (everything but the last lands in the splat target)
*xs, last = [10, 20, 30, 40]
puts last         # 40
puts xs.length    # 3
xs.each { |v| puts v }  # 10 / 20 / 30

# Middle splat
first, *mid, fin = [100, 200, 300, 400, 500]
puts first        # 100
puts fin          # 500
puts mid.length   # 3
mid.each { |v| puts v } # 200 / 300 / 400

# Splat with str_array RHS
s, *rest = ["alpha", "beta", "gamma"]
puts s            # alpha
puts rest.length  # 2
rest.each { |w| puts w }  # beta / gamma

# RHS shorter than fixed targets — splat target empty
p, *q = [42]
puts p            # 42
puts q.length     # 0

# Function returning typed array
def numbers
  [7, 8, 9, 10]
end

n1, *n_rest = numbers
puts n1           # 7
puts n_rest.length  # 3
n_rest.each { |v| puts v }  # 8 / 9 / 10

# Mixed-type destructure: each fixed target gets its own native type
ma, mb, mc = [1, "b", 2.0]
puts ma           # 1
puts mb           # b
puts mc           # 2.0

# Mixed-type with splat: splat target becomes a poly_array
mx, *mmid, mlast = [10, "two", :three, 4.0, "five"]
puts mx           # 10
puts mlast        # five
puts mmid.length  # 3
mmid.each { |mv| p mv }  # "two" / :three / 4.0
