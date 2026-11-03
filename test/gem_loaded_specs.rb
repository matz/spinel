# Gem.loaded_specs: library code reads a gem's version off it and rescues
# when the gem is not loaded (et-orbi's tzinfo-data probe). A compiled
# program activates no gem, so it holds no spec.
v = (Gem.loaded_specs["tzinfo-data-not-installed"].version.to_s rescue nil)
p v
p Gem.loaded_specs.key?("tzinfo-data-not-installed")
p Gem.loaded_specs["tzinfo-data-not-installed"]
