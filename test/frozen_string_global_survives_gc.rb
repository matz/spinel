# Global marking keeps the owner of a frozen String live across collections.
VERSION = "0.8.0".freeze
NAME = ("spinel-" + "rt").freeze
GC.start
GC.start
sum = 0.0
i = 0
while i < 50000
  a = [1.0, 2.0, 3.0, 4.0]
  sum += a[0]
  i += 1
end
GC.start
puts VERSION
puts NAME
puts(sum > 0.0)
