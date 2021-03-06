-- Copyright 2017 Fabricio Ribeiro Toloczko 

-- OpenGPU

-- -- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at

--     http://www.apache.org/licenses/LICENSE-2.0

-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.ogpu_data_record_pkg.all;

entity ogpu_edge_test_testbench is
end ogpu_edge_test_testbench;

architecture ogpu_edge_test_tb1 of ogpu_edge_test_testbench is
	constant clock_period : time := 20 ns;
	signal clock,reset	: std_logic :='0';
	--
	signal edge_test: std_logic :='0';
	signal edge_ready:  std_logic :='0';
	signal edge_mask:   std_logic_vector(0 to 3);
	-- DATAPATH/CONTROL interface signals
	signal quad:  ogpu_quad;
	signal e:  ogpu_edge;
begin
ET1:  entity work.ogpu_quad_edge_test(edge_test_1) port map(
									-- IN
									clock=>clock,reset=>reset,
									d.edge_test=>edge_test,d.e=>e,
									d.quad=>quad,
									-- OUT
									q.edge_ready=>edge_ready,
									q.edge_mask=>edge_mask);
reset_proc: process
	begin
		--reset <= '0';
		--wait for 5*clock_period;
		reset <= '1';
		wait for 5*clock_period;
		reset <= '0';
		wait;
	end process;
		
stim_proc: process
	begin
		edge_test <= '0';
		e.x0<=to_unsigned(1,16);
		e.y0<=to_unsigned(1,16);
		e.x1<=to_unsigned(100,16);
		e.y1<=to_unsigned(100,16);
		
		-- quad at (1,20): at left, under edge
		quad.x0<=to_unsigned(1,16);		quad.x1<=to_unsigned(2,16);
		quad.y0<=to_unsigned(20,16);		quad.y1<=to_unsigned(20,16);
		
		quad.x2<=to_unsigned(1,16);		quad.x3<=to_unsigned(2,16);
		quad.y2<=to_unsigned(21,16);		quad.y3<=to_unsigned(21,16);
		wait for 6*clock_period;
		edge_test <= '1';
		wait for 2*clock_period;
		edge_test <= '0';
		wait for 2*clock_period;
		
		-- other quad (30,30): exactly over edge
		quad.x0<=to_unsigned(30,16);		quad.x1<=to_unsigned(31,16);
		quad.y0<=to_unsigned(30,16);		quad.y1<=to_unsigned(30,16);
		
		quad.x2<=to_unsigned(30,16);		quad.x3<=to_unsigned(31,16);
		quad.y2<=to_unsigned(31,16);		quad.y3<=to_unsigned(31,16);
		wait for 6*clock_period;
		edge_test <= '1';
		wait for 2*clock_period;
		edge_test <= '0';
		wait for 2*clock_period;
		
		-- other quad (60,15): at right, over edge
		quad.x0<=to_unsigned(60,16);		quad.x1<=to_unsigned(61,16);
		quad.y0<=to_unsigned(15,16);		quad.y1<=to_unsigned(15,16);
		
		quad.x2<=to_unsigned(60,16);		quad.x3<=to_unsigned(61,16);
		quad.y2<=to_unsigned(16,16);		quad.y3<=to_unsigned(16,16);
		wait for 6*clock_period;
		edge_test <= '1';
		wait for 2*clock_period;
		edge_test <= '0';
		wait for 2*clock_period;

		
		
		-- The same, but with another edge		
		edge_test <= '0';
		e.x0<=to_unsigned(20,16);
		e.y0<=to_unsigned(10,16);
		e.x1<=to_unsigned(5,16);
		e.y1<=to_unsigned(40,16);
		

		-- quad at (2,3): at left, over edge
		quad.x0<=to_unsigned(2,16);		quad.x1<=to_unsigned(3,16);
		quad.y0<=to_unsigned(3,16);		quad.y1<=to_unsigned(3,16);
		
		quad.x2<=to_unsigned(2,16);		quad.x3<=to_unsigned(3,16);
		quad.y2<=to_unsigned(4,16);		quad.y3<=to_unsigned(4,16);
		wait for 6*clock_period;
		edge_test <= '1';
		wait for 2*clock_period;
		edge_test <= '0';
		wait for 2*clock_period;
		
		-- other quad (60,90): at right, under edge
		quad.x0<=to_unsigned(60,16);		quad.x1<=to_unsigned(61,16);
		quad.y0<=to_unsigned(90,16);		quad.y1<=to_unsigned(90,16);
		
		quad.x2<=to_unsigned(60,16);		quad.x3<=to_unsigned(61,16);
		quad.y2<=to_unsigned(91,16);		quad.y3<=to_unsigned(91,16);
		wait for 6*clock_period;
		edge_test <= '1';
		wait for 2*clock_period;
		edge_test <= '0';
		wait for 2*clock_period;
	wait;
	end process;
								
clock_process: process
	begin
		clock <= not clock;
		wait for clock_period/2;
	end process;
end ogpu_edge_test_tb1;