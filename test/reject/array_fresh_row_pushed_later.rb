# The Array literal holds one row a builtin returns, and a second row is
# pushed onto the local afterwards. Boxing the literal's row as the general
# Array would leave the pushed row an Integer Array, so the String push is
# refused at compile time.
a = ["ab".bytes]
a << "cd".bytes
a[1] << "q"
p a
