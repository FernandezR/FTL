#!/bin/python3
# Pi-hole: A black hole for Internet advertisements
# (c) 2023 Pi-hole, LLC (https://pi-hole.net)
# Network-wide ad blocking via your own hardware.
#
# FTL Engine - auxiliary files
# API test script
#
# This file is copyright under the latest version of the EUPL.
# Please see LICENSE file for your rights under this license.

from enum import Enum
import random
import requests
from typing import List
import json
from hashlib import sha256

url = "http://pi.hole/api/auth"

"""
challenge = requests.get(url).json()["challenge"].encode('ascii')
response = sha256(challenge + b":" + pwhash).hexdigest().encode("ascii")
session = requests.post(url, data = {"response": response}).json()

valid = session["session"]["valid"] # True / False
sid = session["session"]["sid"] # SID string if succesful, null otherwise
"""

class AuthenticationMethods(Enum):
	RANDOM = 0
	HEADER = 1
	BODY = 2
	COOKIE = 3

# Class to query the FTL API
class FTLAPI():

	auth_method = "?"

	def __init__(self, api_url: str):
		self.api_url = api_url
		self.endpoints = {
			"get": [],
			"post": [],
			"put": [],
			"patch": [],
			"delete": []
		}
		self.errors = []
		self.session = None
		self.verbose = False

		# Login to FTL API
		self.login()
		if self.session is None or 'valid' not in self.session or not self.session['valid']:
			raise Exception("Could not login to FTL API")

	def login(self, password: str = None):
		# Get challenge from FTL
		response = self.GET("/api/auth")

		# Check if we are already logged in or authentication is not
		# required
		if response is None:
			raise Exception("No response from FTL API")
		if 'session' in response and response['session']['valid']:
			if 'session' not in response:
				raise Exception("FTL returned invalid challenge item")
			self.session = response["session"]
			return

		pwhash = None
		if password is None:
			# Try to obtain the password hash from pihole.toml
			try:
				with open("/etc/pihole/pihole.toml", "r") as f:
					# Iterate over all lines
					for line in f:
						# Find the line with the password hash
						if line.startswith("    pwhash = "):
							# Remove quotes and whitespace
							line = line.split("=")[1].split("\"")
							if len(line) > 2:
								pwhash = line[1].strip()
							break
			except Exception as e:
				# Could not read pihole.toml, throw an error
				raise Exception("Could not read pihole.toml: " + str(e))
			if pwhash is None:
				# The password hash was not found in pihole.toml, throw an error
				raise Exception("No password hash found in pihole.toml")

		else:
			# Generate password hash
			pwhash = sha256(password.encode("ascii")).hexdigest()
			pwhash = sha256(pwhash.encode("ascii")).hexdigest()
		print("Using password hash: " + pwhash)

		if len(pwhash) != 64:
			raise Exception("Invalid length of password hash")

		# Get the challenge from FTL
		challenge = response["challenge"].encode("ascii")
		response = sha256(challenge + b":" + pwhash.encode("ascii")).hexdigest()
		response = self.POST("/api/auth", {"response": response})
		if 'session' not in response:
			raise Exception("FTL returned invalid challenge item")
		self.session = response["session"]


	def get_jsondata_headers_cookies(self, authenticate: AuthenticationMethods):
		# Add session ID to the request (if any)
		json_data = None
		headers = None
		cookies = None
		if self.session is not None and 'sid' in self.session:
			# Pick a random authentication method if requested
			# Try again if the method comes out as random again
			while authenticate == AuthenticationMethods.RANDOM:
				authenticate = random.choice(list(AuthenticationMethods))

			# Add the session ID to the request
			if authenticate == AuthenticationMethods.HEADER:
				headers = {"X-FTL-SID": self.session['sid']}
			elif authenticate == AuthenticationMethods.BODY:
				json_data = {"sid": self.session['sid'] }
			elif authenticate == AuthenticationMethods.COOKIE:
				cookies = {"sid": self.session['sid'] }

			self.auth_method = authenticate.name

		return json_data, headers, cookies


	# Query the FTL API (GET) and return the response
	def GET(self, uri: str, params: List[str] = [], expected_mimetype: str = "application/json", authenticate: AuthenticationMethods = AuthenticationMethods.BODY):
		self.errors = []
		try:
			# Add parameters to the URI (if any)
			if len(params) > 0:
				uri = uri + "?" + "&".join(params)

			# Get json_data, headers and cookies
			json_data, headers, cookies = self.get_jsondata_headers_cookies(authenticate)

			if self.verbose:
				print("GET " + self.api_url + uri + " with json_data: " + json.dumps(json_data))

			# Query the API
			with requests.get(url = self.api_url + uri, json = json_data, headers=headers, cookies=cookies) as response:
				if self.verbose:
					print(json.dumps(response.json(), indent=4))
				if expected_mimetype == "application/json":
					return response.json()
				else:
					return response.content
		except Exception as e:
			self.errors.append("Exception when GETing from FTL: " + str(e))
			return None


	# Query the FTL API (POST) and return the response
	def POST(self, uri: str, json_data: dict = {}, authenticate: AuthenticationMethods = AuthenticationMethods.HEADER, files = None):
		self.errors = []
		try:
			# Get json_data, headers and cookies
			_, headers, cookies = self.get_jsondata_headers_cookies(authenticate)

			if self.verbose:
				print("POST " + self.api_url + uri + " with json_data: " + json.dumps(json_data))

			# Query the API
			with requests.post(url = self.api_url + uri, json = json_data, files = files, headers=headers, cookies=cookies) as response:
				if self.verbose:
					print(json.dumps(response.json(), indent=4))
				return response.json()
		except Exception as e:
			self.errors.append("Exception when POSTing to FTL: " + str(e))
			return None


	# Query the endpoints from FTL for comparison with the OpenAPI specs
	def get_endpoints(self):
		try:
			# Get all endpoints from FTL and sort them for comparison
			response = self.GET("/api/endpoints")
			for method in response["endpoints"]:
				for endpoint in response["endpoints"][method]:
					self.endpoints[method].append(endpoint["uri"] + endpoint["parameters"])
			for method in self.endpoints:
				self.endpoints[method] = sorted(self.endpoints[method])
		except Exception as e:
			print("Exception when pre-processing endpoints from FTL: " + str(e))
			exit(1)

		return self.endpoints