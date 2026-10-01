# A global among the elements a splat hands to a top-level method's
# parameter that appends: its read is boxed into the gathered Array as a
# fresh handle of its bytes, so the append would not reach the global's
# String. Refused rather than compiled with the append lost (#6179).
def app(a) = (a << "!"; nil)
$g = +"g"
app(*[$g])
p $g
