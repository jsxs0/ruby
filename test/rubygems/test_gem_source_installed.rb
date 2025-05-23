# frozen_string_literal: true

require_relative "helper"
require "rubygems/source"

class TestGemSourceInstalled < Gem::TestCase
  def test_spaceship
    a1 = quick_gem "a", "1"
    util_build_gem a1

    remote    = Gem::Source.new @gem_repo
    specific  = Gem::Source::SpecificFile.new a1.cache_file
    installed = Gem::Source::Installed.new
    local     = Gem::Source::Local.new
    git       = Gem::Source::Git.new "a", "a", nil
    vendor    = Gem::Source::Vendor.new "a"

    assert_equal(0, installed.<=>(installed), "installed <=> installed") # rubocop:disable Lint/BinaryOperatorWithIdenticalOperands

    assert_equal(-1, remote.<=>(installed), "remote <=> installed")
    assert_equal(1, installed.<=>(remote),    "installed <=> remote")

    assert_equal(1, installed.<=>(local),     "installed <=> local")
    assert_equal(-1, local.<=>(installed), "local <=> installed")

    assert_equal(-1, specific.<=>(installed), "specific <=> installed")
    assert_equal(1, installed.<=>(specific),  "installed <=> specific")

    assert_equal(1, git. <=>(installed), "git <=> installed")
    assert_equal(-1, installed.<=>(git), "installed <=> git")

    assert_equal(1, vendor.<=>(installed), "vendor <=> installed")
    assert_equal(-1, installed.<=>(vendor), "installed <=> vendor")
  end

  def test_pretty_print
    local = Gem::Source::Installed.new
    assert_equal "#<Gem::Source::Installed[Installed]>", local.pretty_inspect.gsub(/\s+/, " ").strip
  end
end
