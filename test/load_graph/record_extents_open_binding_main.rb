# A local read through binding reaches the local by a computed name: the
# lambda starts where it is made.
l = -> { p :binding }
binding.local_variable_get(:l).call
