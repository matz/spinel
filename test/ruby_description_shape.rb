# RUBY_DESCRIPTION has the `ruby -v` shape: "<engine> <version> (...)
# [<platform>]", with the version word RUBY_ENGINE_VERSION reports, so a
# script reading the version or platform out of it agrees with the constants.
d = RUBY_DESCRIPTION
p d.start_with?("#{RUBY_ENGINE} #{RUBY_ENGINE_VERSION} (")
p d.include?(RUBY_VERSION)
p d.end_with?("[#{RUBY_PLATFORM}]")
p d[/\A\S+ (\d+\.\d+\.\d+)/, 1] == RUBY_VERSION
p d.frozen?
