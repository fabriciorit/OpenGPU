-- soc_system_dbg_src.vhd

-- Generated using ACDS version 16.0 222

library IEEE;
use IEEE.std_logic_1164.all;
use IEEE.numeric_std.all;

entity soc_system_dbg_src is
	generic (
		sld_auto_instance_index : string  := "YES";
		sld_instance_index      : integer := 0;
		instance_id             : string  := "CLK0";
		probe_width             : integer := 0;
		source_width            : integer := 2;
		source_initial_value    : string  := "0";
		enable_metastability    : string  := "YES"
	);
	port (
		source     : out std_logic_vector(1 downto 0);        --    sources.source
		source_clk : in  std_logic                    := '0'; -- source_clk.clk
		source_ena : in  std_logic                    := '0'
	);
end entity soc_system_dbg_src;

architecture rtl of soc_system_dbg_src is
	component altsource_probe_top is
		generic (
			sld_auto_instance_index : string  := "YES";
			sld_instance_index      : integer := 0;
			instance_id             : string  := "NONE";
			probe_width             : integer := 1;
			source_width            : integer := 1;
			source_initial_value    : string  := "0";
			enable_metastability    : string  := "NO"
		);
		port (
			source     : out std_logic_vector(1 downto 0);        -- source
			source_clk : in  std_logic                    := 'X'; -- clk
			source_ena : in  std_logic                    := 'X'  -- source_ena
		);
	end component altsource_probe_top;

begin

	sld_auto_instance_index_check : if sld_auto_instance_index /= "YES" generate
		assert false report "Supplied generics do not match expected generics" severity Failure;
	end generate;

	sld_instance_index_check : if sld_instance_index /= 0 generate
		assert false report "Supplied generics do not match expected generics" severity Failure;
	end generate;

	instance_id_check : if instance_id /= "CLK0" generate
		assert false report "Supplied generics do not match expected generics" severity Failure;
	end generate;

	probe_width_check : if probe_width /= 0 generate
		assert false report "Supplied generics do not match expected generics" severity Failure;
	end generate;

	source_width_check : if source_width /= 2 generate
		assert false report "Supplied generics do not match expected generics" severity Failure;
	end generate;

	source_initial_value_check : if source_initial_value /= "0" generate
		assert false report "Supplied generics do not match expected generics" severity Failure;
	end generate;

	enable_metastability_check : if enable_metastability /= "YES" generate
		assert false report "Supplied generics do not match expected generics" severity Failure;
	end generate;

	dbg_src : component altsource_probe_top
		generic map (
			sld_auto_instance_index => "YES",
			sld_instance_index      => 0,
			instance_id             => "CLK0",
			probe_width             => 0,
			source_width            => 2,
			source_initial_value    => "0",
			enable_metastability    => "YES"
		)
		port map (
			source     => source,     --    sources.source
			source_clk => source_clk, -- source_clk.clk
			source_ena => '1'         -- (terminated)
		);

end architecture rtl; -- of soc_system_dbg_src
