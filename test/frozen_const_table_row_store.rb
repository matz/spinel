# A store into a row of a frozen constant table changes the row, which
# is not frozen, and leaves the outer Array as it was, as in CRuby.
TABLE = [[3.0, 2.0], [22.0, 20.0]].freeze
IT = [[1, 2], [3, 4]].freeze
def scale(t) = t.map { |r| r.map { |x| x * 2 } }
p scale(TABLE)
TABLE[0][0] = 9.5
puts "a"
IT[0][1] = 20
puts "b"
p TABLE, IT
T2 = [[3.0, 2.0], [22.0, 20.0]].freeze
T2[0][0] += 1
T2[1][1] *= 2
p T2
S = [["a", "b"], ["c"]].freeze
S[0][1] = "z"
S[1] << "d"
p S
M = [[1, 2.0], [:x, "y"]].freeze
M[0][0] = 5
M[1][0] = 6.5
p M
P = [[1, 2], [3, 4]].freeze
P[0][0] = "str"
p P
begin
  T2[0] = [0.0]
rescue => e
  p e.class
end
H = {a: [1.0, 2.0]}.freeze
H[:a][0] = 7.5
p H
G = {a: [1, 2]}.freeze
G[:a][0] = "s"
p G
P2 = [[1, 2], [3, 4]].freeze
r = P2[0]
P2[0][0] = "str"
p r, P2
Q2 = [[1, 2], [3, 4]]
s = Q2[0]
Q2[0][0] = "str"
p s, Q2
p P2.frozen?, P2[0].frozen?
T3 = [[1, 2].freeze, [1.5].freeze].freeze
begin; T3[0][0] = 5; rescue => e; p e.class; end
begin; T3[0][0] = "s"; rescue => e; p e.class; end
begin; T3[1][0] = 2.5; rescue => e; p e.class; end
S3 = ["ab".freeze, +"cd"].freeze
begin; S3[0][0] = "z"; rescue => e; p e.class; end
S3[1][0] = "z"
p S3, T3
T4 = [[1.0, 2.0]].freeze
begin; T4[5][0, 1] = [1]; rescue => e; p e.class; end
begin; T4[5][0..0] = [1]; rescue => e; p e.class; end
N4 = [[1.0, 2.0], "ab"]
begin; N4[7][0, 1] = [1]; rescue => e; p e.class; end
p T4, N4
