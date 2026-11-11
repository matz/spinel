# A call under defined? does not run, and is not split.
a = [1]; e = {}; n = {k: 1}
p defined?(a.push(**e)), defined?(a.sum(**n)), defined?(a.zz_none(**e)), a
