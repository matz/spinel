def absolute = yield.abs
p absolute { -3 }
p absolute { -2.5 }

def negate = yield.-@
p negate { 5 }
p negate { 3.14 }

# Unary minus on a String block is the frozen String itself
def negate_any = yield.-@
p negate_any { "ab" }
p negate_any { 5 }
p negate_any { "ab" }.frozen?

def negate_rev = yield.-@
p negate_rev { 7 }
p negate_rev { "cd" }
