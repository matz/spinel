# A boxed value that follows an Integer down a chain of methods, with no
# Method object anywhere: `near10(0)` types the parameter Integer, and `via`
# hands it a Float and a String one round later. Each type crosses one call
# a round, and the rounds after the re-narrow stopped at a fixed eight: at
# exactly this length the boxed value reached every method but `near0`,
# which kept its Integer parameter, and the call into it turned 1.5 into 1
# and "s" into 0 with nothing raised.
def near0(value) = value
def near1(value) = near0(value)
def near2(value) = near1(value)
def near3(value) = near2(value)
def near4(value) = near3(value)
def near5(value) = near4(value)
def near6(value) = near5(value)
def near7(value) = near6(value)
def near8(value) = near7(value)
def near9(value) = near8(value)
def near10(value) = near9(value)
def via(v) = near10(v)
p near10(0)
p via(1.5)
p via("s")
p near10(2)
