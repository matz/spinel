# The final splat supplies the String variable that would be copied.
def run(s) = yield(k: +"other", **{ k: s })
s = +"s"
p run(s) { |k:| k << "x" }
p s
