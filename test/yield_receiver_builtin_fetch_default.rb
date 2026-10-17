# fetch(i, default) (the 2-argument, with-default form) on a yield
# receiver: the element kind when the default's own type agrees with it
# or is unresolved, poly when it disagrees -- mirrors
# analyze_infer_recv.c's own fetch arm, which needs the default
# argument's own kind to answer correctly (unlike the 1-argument form).
def t = yield.fetch(5, 0)
p t { [1, 2] }
p t { ["a"] }
