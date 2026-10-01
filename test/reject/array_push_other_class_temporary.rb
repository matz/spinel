# A String pushed onto an Integer array the program does not keep in a
# variable (here the Array#map result): the inference widens a variable's
# array to take it, but not a temporary's, so the push is refused rather
# than writing a String into an Integer element.
p [1, 2].map { |x| x }.push("s")
