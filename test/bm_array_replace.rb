# Array#replace: mutating in-place copy from another array.
# Previously a silent no-op for arrays; only String/MutableString worked.

a = [1, 2, 3, 4, 5]
a.replace([10, 20, 30])
puts a.length    # 3
puts a[0]        # 10
puts a[2]        # 30

# Replace into smaller -> larger triggers grow
b = [1]
b.replace([100, 200, 300, 400, 500, 600, 700, 800, 900])
puts b.length    # 9
puts b[8]        # 900

# String array
s = ["a", "b", "c"]
s.replace(["x", "y"])
puts s.length    # 2
puts s[0]        # x
puts s[1]        # y

# Float array
f = [1.0, 2.0, 3.0]
f.replace([7.5, 8.5])
puts f.length    # 2
puts f[0]        # 7.5
puts f[1]        # 8.5

# Mutable string (existing path; <<-promoted)
ms = ""
ms << "hello"
ms.replace("world")
puts ms          # world
