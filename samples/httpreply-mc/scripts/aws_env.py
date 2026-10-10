#!/usr/bin/env python3
"""
Shared AWS settings for the EC2 scripts in this directory.

Every value comes from the environment, with the defaults this project's
test VPC uses. Set the variables to run a script against your own account
and subnet. Nothing here is a credential: the AWS CLI supplies those.

Read the values with:

    export SUBNET_ID=subnet-0123456789abcdef0
    export SG_ID=sg-0123456789abcdef0
    export TARGET_PRIVATE_IP=10.0.1.20
    export CLIENT_PRIVATE_IP=10.0.1.21

The scripts print the settings they will use before they create anything.
"""

import os

AWS_REGION = os.environ.get("AWS_REGION", "us-east-1")
INSTANCE_TYPE = os.environ.get("INSTANCE_TYPE", "c6i.large")
SMOKE_CLIENT_TYPE = os.environ.get("SMOKE_CLIENT_TYPE", "t3.micro")
SUBNET_ID = os.environ.get("SUBNET_ID", "subnet-0e6ba9e8b1e7dcdf0")
SG_ID = os.environ.get("SG_ID", "sg-0ac75f6a09207fcfe")
UBUNTU_AMI = os.environ.get("UBUNTU_AMI", "ami-0045d7fc2ad003464")
GATEWAY_IP = os.environ.get("GATEWAY_IP", "172.31.16.1")
NETMASK = os.environ.get("NETMASK", "255.255.240.0")
TARGET_PRIVATE_IP = os.environ.get("TARGET_PRIVATE_IP", "172.31.16.153")
CLIENT_PRIVATE_IP = os.environ.get("CLIENT_PRIVATE_IP", "172.31.16.161")


def summary():
    """One line that shows the settings a run will use."""
    return (
        f"region={AWS_REGION} instance={INSTANCE_TYPE} client={SMOKE_CLIENT_TYPE} "
        f"subnet={SUBNET_ID} sg={SG_ID} ubuntu_ami={UBUNTU_AMI} "
        f"target_ip={TARGET_PRIVATE_IP} client_ip={CLIENT_PRIVATE_IP} "
        f"gw={GATEWAY_IP} netmask={NETMASK}"
    )
