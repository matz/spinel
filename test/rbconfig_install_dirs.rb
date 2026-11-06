# RbConfig::CONFIG names the installation directories library code compares
# paths against, as Strings under a prefix: a backtrace cleaner silences the
# lines under CONFIG["rubylibdir"] with start_with?, and interns the
# directory with String#-@. Before, the keys were absent and the program
# raised on nil.
cfg = RbConfig::CONFIG
rubylibdir = -cfg["rubylibdir"]
p rubylibdir.frozen?, rubylibdir.start_with?("/"), rubylibdir.end_with?("/" + cfg["ruby_version"])
p cfg["sitelibdir"].include?("/site_ruby/"), cfg["vendorlibdir"].include?("/vendor_ruby/")
p cfg["rubyarchdir"].start_with?(rubylibdir + "/"), cfg["rubyarchdir"].end_with?(cfg["arch"])
p cfg["bindir"].end_with?("/bin"), cfg["libdir"].end_with?("/lib"), cfg["rubylibdir"].start_with?(cfg["rubylibprefix"])
p "app/models/post.rb".start_with?(rubylibdir), cfg["prefix"].is_a?(String)
