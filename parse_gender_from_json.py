#!/usr/bin/env python
import os, sys, json

sex_json = sys.argv[1]

with open (sex_json, 'r') as f:
    data = json.load (f)
    print (data['sex'])

os.sys.exit (0)
